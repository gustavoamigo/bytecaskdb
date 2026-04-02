// Benchmarks comparing ByteCask against LevelDB across:
//  - Put throughput (ops/µs)
//  - Get throughput (ops/µs)
//  - Del throughput (ops/µs)
//  - Range scan (50 items from a key) throughput (scans/µs)
//  - Mixed real-life load: 80% get / 10% put / 10% del (ops/µs)
//  - Jitter: per-iteration latency histogrammed via custom counters (p50/p99)
//
// All benchmarks run in two modes:
//   - nosync: WriteOptions::sync = false  (OS page-cache durability)
//   - sync:   WriteOptions::sync = true   (fdatasync per write)
//
// Keys: generate_prefixed_keys() — UUIDv7-like, prefix-heavy, realistic.
// Values: 1 KiB of random (incompressible) bytes; LevelDB compression disabled.
//   Using compressible fill (e.g. 0xAB * 1024) gives LevelDB an unfair
//   advantage: Snappy collapses 1 KiB to ~15 bytes, fitting the entire dataset
//   in the block cache.  Random bytes eliminate that effect.
//
// ByteCask is opened in a fresh tmpdir per benchmark; LevelDB likewise.
// After each benchmark the directory is removed.

#include <algorithm>
#include <benchmark/benchmark.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// LevelDB
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

// ByteCask (C++20 modules)
import bytecask.engine;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr std::size_t kValueSize = 1024;
static constexpr int kRangeLen = 50;
static constexpr int kRangeLen1000 = 1000;

static const std::size_t kDatasetSize = [] {
  const char *env = std::getenv("BC_DATASET_SIZE");
  if (env && *env) {
    return static_cast<std::size_t>(std::stoul(env));
  }
  return std::size_t{50'000};
}();

static constexpr std::size_t kMaxSamples = 1'000'000;

// ---------------------------------------------------------------------------
// Key / value helpers
// ---------------------------------------------------------------------------

namespace {

auto generate_prefixed_keys(std::size_t n) -> std::vector<std::string> {
  static constexpr std::array prefixes = {
      "user::", "order::", "session::", "invoice::", "product::"};
  std::vector<std::string> keys;
  keys.reserve(n);
  const auto per_prefix = n / prefixes.size();

  for (const auto *pfx : prefixes) {
    for (std::size_t i = 0; i < per_prefix; ++i) {
      std::ostringstream oss;
      oss << pfx << "018f6e2c-" << std::hex << std::setfill('0') << std::setw(4)
          << (i >> 16) << "-7000-8000-" << std::setw(12) << (i & 0xFFFFFFFFULL);
      keys.push_back(oss.str());
    }
  }
  return keys;
}

auto make_value() -> std::vector<std::byte> {
  std::mt19937 rng{0x1234ABCD};
  std::uniform_int_distribution<unsigned int> dist{0, 255};
  std::vector<std::byte> v(kValueSize);
  for (auto &b : v)
    b = static_cast<std::byte>(dist(rng));
  return v;
}

auto bc_key(const std::string &s) -> bytecask::BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
}

auto bc_val(const std::vector<std::byte> &v) -> bytecask::BytesView {
  return std::span<const std::byte>{v.data(), v.size()};
}

auto ldb_slice(const std::string &s) -> leveldb::Slice {
  return {s.data(), s.size()};
}

auto ldb_val_slice(const std::vector<std::byte> &v) -> leveldb::Slice {
  return {reinterpret_cast<const char *>(v.data()), v.size()};
}

// ---------------------------------------------------------------------------
// Jitter helpers
// ---------------------------------------------------------------------------

auto percentile(std::vector<double> &samples, double pct) -> double {
  if (samples.empty())
    return 0.0;
  std::sort(samples.begin(), samples.end());
  const auto idx = static_cast<std::size_t>(
      pct / 100.0 * static_cast<double>(samples.size() - 1));
  return samples[idx];
}

void attach_jitter(benchmark::State &state, std::vector<double> &samples) {
  state.counters["lat_p50_ns"] = benchmark::Counter(
      percentile(samples, 50.0), benchmark::Counter::kAvgIterations);
  state.counters["lat_p99_ns"] = benchmark::Counter(
      percentile(samples, 99.0), benchmark::Counter::kAvgIterations);
}

// ---------------------------------------------------------------------------
// Temporary directory RAII
// ---------------------------------------------------------------------------

struct TmpDir {
  std::filesystem::path path;

  explicit TmpDir(std::string_view name) {
    path = std::filesystem::temp_directory_path() /
           ("engine_bench_" + std::string{name} + "_" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(path);
  }

  ~TmpDir() {
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  }

  TmpDir(const TmpDir &) = delete;
  TmpDir &operator=(const TmpDir &) = delete;
  TmpDir(TmpDir &&o) noexcept : path{std::move(o.path)} { o.path.clear(); }
  TmpDir &operator=(TmpDir &&) = delete;
};

// ===========================================================================
// Engine adapters — normalize each engine's API for generic benchmarks.
// ===========================================================================

struct BcAdapter {
  struct Db {
    TmpDir dir;
    bytecask::Bytecask engine;

    Db(std::string_view tag, const std::vector<std::string> *populate_keys,
       const std::vector<std::byte> *populate_val)
        : dir{tag}, engine{bytecask::Bytecask::open(dir.path)} {
      if (populate_keys) {
        bytecask::WriteOptions wo;
        wo.sync = false;
        for (const auto &k : *populate_keys)
          engine.put(wo, bc_key(k), bc_val(*populate_val));
      }
    }
  };

  static auto open_empty(std::string_view tag) -> Db {
    return Db{tag, nullptr, nullptr};
  }

  static auto open_populated(std::string_view tag,
                             const std::vector<std::string> &keys,
                             const std::vector<std::byte> &val) -> Db {
    return Db{tag, &keys, &val};
  }

  static void put(Db &db, const std::string &k, const std::vector<std::byte> &v,
                  bool sync) {
    bytecask::WriteOptions wo;
    wo.sync = sync;
    db.engine.put(wo, bc_key(k), bc_val(v));
  }

  static void get(Db &db, const std::string &k) {
    bytecask::ReadOptions ro;
    bytecask::Bytes value;
    auto found = db.engine.get(ro, bc_key(k), value);
    benchmark::DoNotOptimize(found);
    benchmark::DoNotOptimize(value.data());
  }

  static void del(Db &db, const std::string &k, bool sync) {
    bytecask::WriteOptions wo;
    wo.sync = sync;
    std::ignore = db.engine.del(wo, bc_key(k));
  }

  static void range(Db &db, const std::string &k, int limit) {
    bytecask::ReadOptions ro;
    auto range = db.engine.iter_from(ro, bc_key(k));
    int count = 0;
    for (auto it = range.begin(); it != range.end() && count < limit;
         ++it, ++count) {
      auto entry = *it;
      benchmark::DoNotOptimize(entry);
    }
  }
};

template <bool UseCache = true> struct LdbAdapter {
  struct Db {
    TmpDir dir;
    leveldb::DB *raw{nullptr};

    Db(std::string_view tag, const std::vector<std::string> *populate_keys,
       const std::vector<std::byte> *populate_val)
        : dir{tag} {
      leveldb::Options opts;
      opts.create_if_missing = true;
      opts.compression = leveldb::kNoCompression;
      if constexpr (!UseCache)
        opts.block_cache = nullptr;
      auto s = leveldb::DB::Open(opts, dir.path.string(), &raw);
      if (!s.ok())
        throw std::runtime_error{"LevelDB open failed: " + s.ToString()};

      if (populate_keys) {
        leveldb::WriteOptions wo;
        wo.sync = false;
        for (const auto &k : *populate_keys) {
          s = raw->Put(wo, ldb_slice(k), ldb_val_slice(*populate_val));
          if (!s.ok())
            throw std::runtime_error{"LevelDB put failed: " + s.ToString()};
        }
      }
    }

    ~Db() { delete raw; }
    Db(const Db &) = delete;
    Db &operator=(const Db &) = delete;
  };

  static auto open_empty(std::string_view tag) -> Db {
    return Db{tag, nullptr, nullptr};
  }

  static auto open_populated(std::string_view tag,
                             const std::vector<std::string> &keys,
                             const std::vector<std::byte> &val) -> Db {
    return Db{tag, &keys, &val};
  }

  static void put(Db &db, const std::string &k, const std::vector<std::byte> &v,
                  bool sync) {
    leveldb::WriteOptions wo;
    wo.sync = sync;
    auto s = db.raw->Put(wo, ldb_slice(k), ldb_val_slice(v));
    benchmark::DoNotOptimize(s);
  }

  static void get(Db &db, const std::string &k) {
    leveldb::ReadOptions ro;
    if constexpr (!UseCache)
      ro.fill_cache = false;
    std::string value;
    auto s = db.raw->Get(ro, ldb_slice(k), &value);
    benchmark::DoNotOptimize(value);
  }

  static void del(Db &db, const std::string &k, bool sync) {
    leveldb::WriteOptions wo;
    wo.sync = sync;
    auto s = db.raw->Delete(wo, ldb_slice(k));
    benchmark::DoNotOptimize(s);
  }

  static void range(Db &db, const std::string &k, int limit) {
    leveldb::ReadOptions ro;
    if constexpr (!UseCache)
      ro.fill_cache = false;
    std::unique_ptr<leveldb::Iterator> it{db.raw->NewIterator(ro)};
    it->Seek(ldb_slice(k));
    int count = 0;
    for (; it->Valid() && count < limit; it->Next(), ++count) {
      benchmark::DoNotOptimize(it->value());
    }
  }
};

// ===========================================================================
// Generic benchmark templates
// ===========================================================================

// ──────────────────────────── Put ────────────────────────────────────────────

template <typename A, bool Sync> void BM_Put(benchmark::State &state) {
  auto db = A::open_empty("put");
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();

  std::size_t idx = 0;
  std::vector<double> samples;
  if constexpr (Sync)
    samples.reserve(std::min(kMaxSamples, std::size_t{10000}));
  else
    samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    A::put(db, k, val, Sync);
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
  state.counters["ops_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ──────────────────────────── Get ────────────────────────────────────────────

template <typename A> void BM_Get(benchmark::State &state) {
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  auto db = A::open_populated("get", keys, val);

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    A::get(db, k);
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
  state.counters["ops_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ──────────────────────────── Del ────────────────────────────────────────────

template <typename A, bool Sync> void BM_Del(benchmark::State &state) {
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  auto db = A::open_populated("del", keys, val);

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    // Re-insert if we've cycled through all keys so del always has something.
    if (idx > 0 && idx % keys.size() == 0) {
      for (const auto &rk : keys)
        A::put(db, rk, val, false);
    }
    const auto t0 = std::chrono::high_resolution_clock::now();
    A::del(db, k, Sync);
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
  state.counters["ops_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ──────────────────────────── Range ──────────────────────────────────────────

template <typename A, int RangeLen> void BM_Range(benchmark::State &state) {
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  auto db = A::open_populated("range", keys, val);

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &start_key = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    A::range(db, start_key, RangeLen);
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * RangeLen);
  state.counters["scans_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ──────────────────────────── Mixed ──────────────────────────────────────────
// 80% get / 10% put / 10% del

template <typename A, bool Sync> void BM_Mixed(benchmark::State &state) {
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  auto db = A::open_populated("mixed", keys, val);

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto op = idx % 10; // 0–7: get, 8: put, 9: del
    const auto t0 = std::chrono::high_resolution_clock::now();
    if (op <= 7) {
      A::get(db, k);
    } else if (op == 8) {
      A::put(db, k, val, Sync);
    } else {
      A::del(db, k, Sync);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
  state.counters["ops_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ===========================================================================
// Registration
// clang-format off
// ===========================================================================

using Bc  = BcAdapter;
using Ldb = LdbAdapter<true>;
using LdbNC = LdbAdapter<false>;

// --- ByteCask ---
BENCHMARK(BM_Put<Bc, false>)          ->Name("ByteCask/Put/NoSync");
BENCHMARK(BM_Put<Bc, true>)           ->Name("ByteCask/Put/Sync")      ->Iterations(5000);
BENCHMARK(BM_Del<Bc, false>)          ->Name("ByteCask/Del/NoSync");
BENCHMARK(BM_Del<Bc, true>)           ->Name("ByteCask/Del/Sync")      ->Iterations(5000);
BENCHMARK(BM_Get<Bc>)                 ->Name("ByteCask/Get");
BENCHMARK(BM_Range<Bc, kRangeLen>)    ->Name("ByteCask/Range50");
BENCHMARK(BM_Mixed<Bc, true>)         ->Name("ByteCask/Mixed/Sync")    ->Iterations(5000);
BENCHMARK(BM_Mixed<Bc, false>)        ->Name("ByteCask/Mixed/NoSync");

// --- LevelDB ---
BENCHMARK(BM_Put<Ldb, false>)          ->Name("LevelDB/Put/NoSync");
BENCHMARK(BM_Put<Ldb, true>)           ->Name("LevelDB/Put/Sync")     ->Iterations(5000);
BENCHMARK(BM_Del<Ldb, false>)          ->Name("LevelDB/Del/NoSync");
BENCHMARK(BM_Del<Ldb, true>)           ->Name("LevelDB/Del/Sync")     ->Iterations(5000);
BENCHMARK(BM_Get<Ldb>)                 ->Name("LevelDB/Get");
BENCHMARK(BM_Range<Ldb, kRangeLen>)    ->Name("LevelDB/Range50");
BENCHMARK(BM_Mixed<Ldb, true>)         ->Name("LevelDB/Mixed/Sync")   ->Iterations(5000);
BENCHMARK(BM_Mixed<Ldb, false>)        ->Name("LevelDB/Mixed/NoSync");

// clang-format on

} // namespace

// Publish dataset_size into the JSON context section so the CSV tracking
// script can record it as a column alongside git_commit and timestamp.
// NOLINTNEXTLINE(cert-err58-cpp)
const bool kDatasetSizeContext = [] {
  benchmark::AddCustomContext("dataset_size", std::to_string(kDatasetSize));
  return true;
}();

BENCHMARK_MAIN();
