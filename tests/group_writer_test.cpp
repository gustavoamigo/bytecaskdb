#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

import bytecask.engine;
import bytecask.group_writer;

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
                 "gw_test_{}",
                 std::chrono::system_clock::now().time_since_epoch().count())} {
    std::filesystem::create_directories(path);
  }

  ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

// ---------------------------------------------------------------------------
// Test GW-1: put round-trip via GroupWriter
// ---------------------------------------------------------------------------
TEST_CASE("GroupWriter put and get round-trip", "[group_writer]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");
  bytecask::GroupWriter gw{db};

  gw.put(to_bytes("key1"), to_bytes("value1"));

  const auto result = db.get({}, to_bytes("key1"));
  REQUIRE(result.has_value());
  CHECK(to_string(*result) == "value1");
}

// ---------------------------------------------------------------------------
// Test GW-2: del returns false for absent key, true for present key
// ---------------------------------------------------------------------------
TEST_CASE("GroupWriter del absent key returns false", "[group_writer]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");
  bytecask::GroupWriter gw{db};

  CHECK_FALSE(gw.del(to_bytes("missing")));
}

TEST_CASE("GroupWriter del present key returns true", "[group_writer]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");
  bytecask::GroupWriter gw{db};

  gw.put(to_bytes("k"), to_bytes("v"));
  CHECK(gw.del(to_bytes("k")));
  CHECK_FALSE(db.get({}, to_bytes("k")).has_value());
}

// ---------------------------------------------------------------------------
// Test GW-3: apply_batch via GroupWriter applies all operations
// ---------------------------------------------------------------------------
TEST_CASE("GroupWriter apply_batch mixed operations", "[group_writer]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");
  bytecask::GroupWriter gw{db};

  db.put({}, to_bytes("del"), to_bytes("gone"));

  bytecask::Batch batch;
  batch.put(to_bytes("a"), to_bytes("alpha"));
  batch.put(to_bytes("b"), to_bytes("beta"));
  batch.del(to_bytes("del"));
  gw.apply_batch(std::move(batch));

  REQUIRE(db.get({}, to_bytes("a")).has_value());
  CHECK(to_string(*db.get({}, to_bytes("a"))) == "alpha");
  REQUIRE(db.get({}, to_bytes("b")).has_value());
  CHECK(to_string(*db.get({}, to_bytes("b"))) == "beta");
  CHECK_FALSE(db.get({}, to_bytes("del")).has_value());
}

// ---------------------------------------------------------------------------
// Test GW-4: multiple sequential puts are all durable
// ---------------------------------------------------------------------------
TEST_CASE("GroupWriter multiple sequential puts", "[group_writer]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");
  bytecask::GroupWriter gw{db};

  constexpr int kN = 50;
  for (int i = 0; i < kN; ++i) {
    auto k = std::format("key{:04d}", i);
    auto v = std::format("val{:04d}", i);
    gw.put(to_bytes(k), to_bytes(v));
  }

  for (int i = 0; i < kN; ++i) {
    auto k = std::format("key{:04d}", i);
    auto v = std::format("val{:04d}", i);
    const auto r = db.get({}, to_bytes(k));
    REQUIRE(r.has_value());
    CHECK(to_string(*r) == v);
  }
}

// ---------------------------------------------------------------------------
// Test GW-5: concurrent puts from N threads — all entries readable afterwards
// ---------------------------------------------------------------------------
TEST_CASE("GroupWriter concurrent puts from multiple threads",
          "[group_writer][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");
  bytecask::GroupWriter gw{db};

  constexpr int kThreads = 4;
  constexpr int kPerThread = 25;

  std::vector<std::thread> threads;
  threads.reserve(kThreads);

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        auto k = std::format("t{:02d}k{:04d}", t, i);
        auto v = std::format("t{:02d}v{:04d}", t, i);
        gw.put(to_bytes(k), to_bytes(v));
      }
    });
  }

  for (auto &th : threads)
    th.join();

  // Every key written must be readable.
  int found = 0;
  for (int t = 0; t < kThreads; ++t) {
    for (int i = 0; i < kPerThread; ++i) {
      auto k = std::format("t{:02d}k{:04d}", t, i);
      auto v = std::format("t{:02d}v{:04d}", t, i);
      const auto r = db.get({}, to_bytes(k));
      if (r.has_value() && to_string(*r) == v)
        ++found;
    }
  }
  CHECK(found == kThreads * kPerThread);
}

// ---------------------------------------------------------------------------
// Test GW-6: concurrent mixed ops (put + del) from N threads
// ---------------------------------------------------------------------------
TEST_CASE("GroupWriter concurrent mixed put and del",
          "[group_writer][concurrency]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");
  bytecask::GroupWriter gw{db};

  // Pre-populate keys that del threads will remove.
  constexpr int kKeys = 20;
  for (int i = 0; i < kKeys; ++i) {
    db.put({}, to_bytes(std::format("shared{:04d}", i)), to_bytes("initial"));
  }

  std::atomic<int> puts_done{0};
  std::atomic<int> dels_done{0};

  auto put_thread = [&] {
    for (int i = 0; i < kKeys; ++i) {
      gw.put(to_bytes(std::format("new{:04d}", i)), to_bytes("newval"));
      ++puts_done;
    }
  };

  auto del_thread = [&] {
    for (int i = 0; i < kKeys; ++i) {
      std::ignore = gw.del(to_bytes(std::format("shared{:04d}", i)));
      ++dels_done;
    }
  };

  std::thread t1{put_thread};
  std::thread t2{del_thread};
  t1.join();
  t2.join();

  CHECK(puts_done == kKeys);
  CHECK(dels_done == kKeys);

  // All new keys must exist.
  for (int i = 0; i < kKeys; ++i) {
    CHECK(db.contains_key(to_bytes(std::format("new{:04d}", i))));
  }
}

// ---------------------------------------------------------------------------
// Test GW-7: GroupWriter and direct engine writes are mutually compatible
// ---------------------------------------------------------------------------
TEST_CASE("GroupWriter and direct engine writes coexist", "[group_writer]") {
  TempDir td;
  auto db = bytecask::Bytecask::open(td.path / "db");
  bytecask::GroupWriter gw{db};

  db.put({}, to_bytes("direct"), to_bytes("d"));
  gw.put(to_bytes("grouped"), to_bytes("g"));
  db.put({}, to_bytes("direct2"), to_bytes("d2"));
  gw.put(to_bytes("grouped2"), to_bytes("g2"));

  CHECK(to_string(*db.get({}, to_bytes("direct"))) == "d");
  CHECK(to_string(*db.get({}, to_bytes("grouped"))) == "g");
  CHECK(to_string(*db.get({}, to_bytes("direct2"))) == "d2");
  CHECK(to_string(*db.get({}, to_bytes("grouped2"))) == "g2");
}
