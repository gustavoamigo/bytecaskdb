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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
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

// Value payload size: chosen to be representative of real-world small record.
static constexpr std::size_t kValueSize = 1024;

// Range scan length: number of consecutive entries to read per operation.
static constexpr int kRangeLen = 50;

// Number of prefixed keys to pre-populate the database with.
// Override at runtime via the BC_DATASET_SIZE environment variable
// (set automatically by run_engine_bench.py).
// Default: 50 000 (light run). Pass --full to run_engine_bench.py for 1 000
// 000.
static const std::size_t kDatasetSize = [] {
  const char *env = std::getenv("BC_DATASET_SIZE");
  if (env && *env) {
    return static_cast<std::size_t>(std::stoul(env));
  }
  return std::size_t{50'000};
}();

// How many latency samples to collect for jitter counters.
// Limited by benchmark iteration count; we cap at 1 M to bound memory.
static constexpr std::size_t kMaxSamples = 1'000'000;

// ---------------------------------------------------------------------------
// Key / value helpers
// ---------------------------------------------------------------------------

namespace {

// Mirrors generate_prefixed_keys from map_bench.cpp.
// Produces keys like: user::018f6e2c-0001-7000-8000-00000000002a
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

// Returns a 1 KiB buffer of pseudo-random bytes (seeded deterministically).
// Incompressible by Snappy/zlib, so LevelDB stores and reads the full 1 KiB
// just like ByteCask does — giving both engines the same I/O workload.
auto make_value() -> std::vector<std::byte> {
  std::mt19937 rng{0x1234ABCD};
  std::uniform_int_distribution<unsigned int> dist{0, 255};
  std::vector<std::byte> v(kValueSize);
  for (auto &b : v)
    b = static_cast<std::byte>(dist(rng));
  return v;
}

// ByteCask view helpers
auto bc_key(const std::string &s) -> bytecask::BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
}

auto bc_val(const std::vector<std::byte> &v) -> bytecask::BytesView {
  return std::span<const std::byte>{v.data(), v.size()};
}

// LevelDB slice helpers
auto ldb_slice(const std::string &s) -> leveldb::Slice {
  return {s.data(), s.size()};
}

auto ldb_val_slice(const std::vector<std::byte> &v) -> leveldb::Slice {
  return {reinterpret_cast<const char *>(v.data()), v.size()};
}

// ---------------------------------------------------------------------------
// p50/p99 from a sorted sample vector.
// ---------------------------------------------------------------------------
auto percentile(std::vector<double> &samples, double pct) -> double {
  if (samples.empty())
    return 0.0;
  std::sort(samples.begin(), samples.end());
  const auto idx = static_cast<std::size_t>(
      pct / 100.0 * static_cast<double>(samples.size() - 1));
  return samples[idx];
}

// Attaches p50/p99 latency counters (nanoseconds) to a benchmark state.
// `samples` holds per-iteration durations in nanoseconds.
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
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  TmpDir(const TmpDir &) = delete;
  TmpDir &operator=(const TmpDir &) = delete;
};

// ---------------------------------------------------------------------------
// Pre-populated stores returned ready for read/range benchmarks.
// ---------------------------------------------------------------------------

struct BcStore {
  TmpDir dir;
  bytecask::Bytecask db;

  explicit BcStore(bool sync = false)
      : dir{"bc"}, db{bytecask::Bytecask::open(dir.path)} {
    auto keys = generate_prefixed_keys(kDatasetSize);
    auto val = make_value();
    bytecask::WriteOptions wo;
    wo.sync = sync;
    for (const auto &k : keys)
      db.put(wo, bc_key(k), bc_val(val));
  }
};

// LevelDB with default (LRU) block cache.
struct LdbStore {
  TmpDir dir;
  leveldb::DB *db{nullptr};

  explicit LdbStore(bool sync = false) : dir{"ldb"} {
    leveldb::Options opts;
    opts.create_if_missing = true;
    opts.compression = leveldb::kNoCompression;
    leveldb::Status s = leveldb::DB::Open(opts, dir.path.string(), &db);
    if (!s.ok())
      throw std::runtime_error{"LevelDB open failed: " + s.ToString()};

    auto keys = generate_prefixed_keys(kDatasetSize);
    auto val = make_value();
    leveldb::WriteOptions wo;
    wo.sync = sync;
    for (const auto &k : keys) {
      s = db->Put(wo, ldb_slice(k), ldb_val_slice(val));
      if (!s.ok())
        throw std::runtime_error{"LevelDB put failed: " + s.ToString()};
    }
  }

  ~LdbStore() { delete db; }

  LdbStore(const LdbStore &) = delete;
  LdbStore &operator=(const LdbStore &) = delete;
};

// LevelDB with block cache disabled: opts.block_cache = nullptr disables the
// default 8 MB LRU cache so every read goes straight to disk (via the OS page
// cache), matching ByteCask's read path asymptotically.
struct LdbStore_NoCache {
  TmpDir dir;
  leveldb::DB *db{nullptr};

  explicit LdbStore_NoCache() : dir{"ldb_nocache"} {
    leveldb::Options opts;
    opts.create_if_missing = true;
    opts.compression = leveldb::kNoCompression;
    opts.block_cache = nullptr; // disable default 8 MB LRU cache
    leveldb::Status s = leveldb::DB::Open(opts, dir.path.string(), &db);
    if (!s.ok())
      throw std::runtime_error{"LevelDB open failed: " + s.ToString()};

    auto keys = generate_prefixed_keys(kDatasetSize);
    auto val = make_value();
    leveldb::WriteOptions wo;
    wo.sync = false;
    for (const auto &k : keys) {
      s = db->Put(wo, ldb_slice(k), ldb_val_slice(val));
      if (!s.ok())
        throw std::runtime_error{"LevelDB put failed: " + s.ToString()};
    }
  }

  ~LdbStore_NoCache() { delete db; }

  LdbStore_NoCache(const LdbStore_NoCache &) = delete;
  LdbStore_NoCache &operator=(const LdbStore_NoCache &) = delete;
};

} // namespace

// ===========================================================================
// ── ByteCask Benchmarks ─────────────────────────────────────────────────────
// ===========================================================================

// ──────────────────────────── Put ────────────────────────────────────────────

static void BC_Put_NoSync(benchmark::State &state) {
  TmpDir dir{"bc_put_nosync"};
  auto db = bytecask::Bytecask::open(dir.path);
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  bytecask::WriteOptions wo;
  wo.sync = false;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    db.put(wo, bc_key(k), bc_val(val));
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

static void BC_Put_Sync(benchmark::State &state) {
  TmpDir dir{"bc_put_sync"};
  auto db = bytecask::Bytecask::open(dir.path);
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  bytecask::WriteOptions wo;
  wo.sync = true;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(std::min(kMaxSamples, static_cast<std::size_t>(10000)));

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    db.put(wo, bc_key(k), bc_val(val));
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

static void BC_Get(benchmark::State &state) {
  BcStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  bytecask::ReadOptions ro;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto v = store.db.get(ro, bc_key(k));
    const auto t1 = std::chrono::high_resolution_clock::now();
    benchmark::DoNotOptimize(v);
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

// ──────────────────────────── Del ─────────────────────────────────────────

static void BC_Del_NoSync(benchmark::State &state) {
  // Re-populate between measurement rounds to avoid deleting all keys.
  // We clone the store fresh per *outer* benchmark run, not per iteration.
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  bytecask::WriteOptions wo_put;
  wo_put.sync = false;
  bytecask::WriteOptions wo_del;
  wo_del.sync = false;

  TmpDir dir{"bc_del_nosync"};
  auto db = bytecask::Bytecask::open(dir.path);
  for (const auto &k : keys)
    db.put(wo_put, bc_key(k), bc_val(val));

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    // Re-insert if we've cycled through all keys so del always has something.
    if (idx > 0 && idx % keys.size() == 0) {
      for (const auto &rk : keys)
        db.put(wo_put, bc_key(rk), bc_val(val));
    }
    const auto t0 = std::chrono::high_resolution_clock::now();
    std::ignore = db.del(wo_del, bc_key(k));
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

// ──────────────────────────── Range ───────────────────────────────────────

static void BC_Range50(benchmark::State &state) {
  BcStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  bytecask::ReadOptions ro;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &start_key = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto range = store.db.iter_from(ro, bc_key(start_key));
    int count = 0;
    for (auto it = range.begin(); it != range.end() && count < kRangeLen;
         ++it, ++count) {
      auto entry = *it;
      benchmark::DoNotOptimize(entry);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRangeLen);
  state.counters["scans_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

static constexpr int kRangeLen1000 = 1000;

static void BC_Range1000(benchmark::State &state) {
  BcStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  bytecask::ReadOptions ro;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &start_key = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto range = store.db.iter_from(ro, bc_key(start_key));
    int count = 0;
    for (auto it = range.begin(); it != range.end() && count < kRangeLen1000;
         ++it, ++count) {
      auto entry = *it;
      benchmark::DoNotOptimize(entry);
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          kRangeLen1000);
  state.counters["scans_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ──────────────────────────── Mixed ───────────────────────────────────────
// 80% get / 10% put / 10% del

static void BC_Mixed_Sync(benchmark::State &state) {
  BcStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  bytecask::ReadOptions ro;
  bytecask::WriteOptions wo;
  wo.sync = true;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto op = idx % 10; // 0–7: get, 8: put, 9: del
    const auto t0 = std::chrono::high_resolution_clock::now();
    if (op <= 7) {
      auto v = store.db.get(ro, bc_key(k));
      benchmark::DoNotOptimize(v);
    } else if (op == 8) {
      store.db.put(wo, bc_key(k), bc_val(val));
    } else {
      std::ignore = store.db.del(wo, bc_key(k));
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

static void BC_Mixed_NoSync(benchmark::State &state) {
  BcStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  bytecask::ReadOptions ro;
  bytecask::WriteOptions wo;
  wo.sync = false;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto op = idx % 10; // 0–7: get, 8: put, 9: del
    const auto t0 = std::chrono::high_resolution_clock::now();
    if (op <= 7) {
      auto v = store.db.get(ro, bc_key(k));
      benchmark::DoNotOptimize(v);
    } else if (op == 8) {
      store.db.put(wo, bc_key(k), bc_val(val));
    } else {
      std::ignore = store.db.del(wo, bc_key(k));
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
// ── LevelDB Benchmarks ──────────────────────────────────────────────────────
// ===========================================================================

// ──────────────────────────── Put ────────────────────────────────────────────

static void LDB_Put_NoSync(benchmark::State &state) {
  TmpDir dir{"ldb_put_nosync"};
  leveldb::DB *db = nullptr;
  leveldb::Options opts;
  opts.create_if_missing = true;
  opts.compression = leveldb::kNoCompression;
  {
    auto s = leveldb::DB::Open(opts, dir.path.string(), &db);
    if (!s.ok())
      throw std::runtime_error{"LevelDB open: " + s.ToString()};
  }

  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  leveldb::WriteOptions wo;
  wo.sync = false;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto s = db->Put(wo, ldb_slice(k), ldb_val_slice(val));
    const auto t1 = std::chrono::high_resolution_clock::now();
    benchmark::DoNotOptimize(s);
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  delete db;
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
  state.counters["ops_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

static void LDB_Put_Sync(benchmark::State &state) {
  TmpDir dir{"ldb_put_sync"};
  leveldb::DB *db = nullptr;
  leveldb::Options opts;
  opts.create_if_missing = true;
  opts.compression = leveldb::kNoCompression;
  {
    auto s = leveldb::DB::Open(opts, dir.path.string(), &db);
    if (!s.ok())
      throw std::runtime_error{"LevelDB open: " + s.ToString()};
  }

  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  leveldb::WriteOptions wo;
  wo.sync = true;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(std::min(kMaxSamples, static_cast<std::size_t>(10000)));

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto s = db->Put(wo, ldb_slice(k), ldb_val_slice(val));
    const auto t1 = std::chrono::high_resolution_clock::now();
    benchmark::DoNotOptimize(s);
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  delete db;
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
  state.counters["ops_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ──────────────────────────── Get ────────────────────────────────────────────

static void LDB_Get(benchmark::State &state) {
  LdbStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  leveldb::ReadOptions ro;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    std::string value;
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto s = store.db->Get(ro, ldb_slice(k), &value);
    const auto t1 = std::chrono::high_resolution_clock::now();
    benchmark::DoNotOptimize(value);
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

// ──────────────────────────── Del ─────────────────────────────────────────

static void LDB_Del_NoSync(benchmark::State &state) {
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();

  TmpDir dir{"ldb_del_nosync"};
  leveldb::DB *db = nullptr;
  leveldb::Options opts;
  opts.create_if_missing = true;
  opts.compression = leveldb::kNoCompression;
  {
    auto s = leveldb::DB::Open(opts, dir.path.string(), &db);
    if (!s.ok())
      throw std::runtime_error{"LevelDB open: " + s.ToString()};
  }

  leveldb::WriteOptions wo_put;
  wo_put.sync = false;
  for (const auto &k : keys)
    db->Put(wo_put, ldb_slice(k), ldb_val_slice(val));

  leveldb::WriteOptions wo_del;
  wo_del.sync = false;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    if (idx > 0 && idx % keys.size() == 0) {
      for (const auto &rk : keys)
        db->Put(wo_put, ldb_slice(rk), ldb_val_slice(val));
    }
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto s = db->Delete(wo_del, ldb_slice(k));
    const auto t1 = std::chrono::high_resolution_clock::now();
    benchmark::DoNotOptimize(s);
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  delete db;
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
  state.counters["ops_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ──────────────────────────── Range ───────────────────────────────────────

static void LDB_Range50(benchmark::State &state) {
  LdbStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  leveldb::ReadOptions ro;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &start_key = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    std::unique_ptr<leveldb::Iterator> it{store.db->NewIterator(ro)};
    it->Seek(ldb_slice(start_key));
    int count = 0;
    for (; it->Valid() && count < kRangeLen; it->Next(), ++count) {
      benchmark::DoNotOptimize(it->value());
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRangeLen);
  state.counters["scans_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

static void LDB_Range1000(benchmark::State &state) {
  LdbStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  leveldb::ReadOptions ro;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &start_key = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    std::unique_ptr<leveldb::Iterator> it{store.db->NewIterator(ro)};
    it->Seek(ldb_slice(start_key));
    int count = 0;
    for (; it->Valid() && count < kRangeLen1000; it->Next(), ++count) {
      benchmark::DoNotOptimize(it->value());
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          kRangeLen1000);
  state.counters["scans_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ──────────────────────────── Mixed ───────────────────────────────────────

static void LDB_Mixed_NoSync(benchmark::State &state) {
  LdbStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  leveldb::ReadOptions ro;
  leveldb::WriteOptions wo;
  wo.sync = false;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto op = idx % 10;
    const auto t0 = std::chrono::high_resolution_clock::now();
    if (op <= 7) {
      std::string value;
      auto s = store.db->Get(ro, ldb_slice(k), &value);
      benchmark::DoNotOptimize(value);
    } else if (op == 8) {
      auto s = store.db->Put(wo, ldb_slice(k), ldb_val_slice(val));
      benchmark::DoNotOptimize(s);
    } else {
      auto s = store.db->Delete(wo, ldb_slice(k));
      benchmark::DoNotOptimize(s);
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

static void LDB_Mixed_Sync(benchmark::State &state) {
  LdbStore store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  leveldb::ReadOptions ro;
  leveldb::WriteOptions wo;
  wo.sync = true;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto op = idx % 10;
    const auto t0 = std::chrono::high_resolution_clock::now();
    if (op <= 7) {
      std::string value;
      auto s = store.db->Get(ro, ldb_slice(k), &value);
      benchmark::DoNotOptimize(value);
    } else if (op == 8) {
      auto s = store.db->Put(wo, ldb_slice(k), ldb_val_slice(val));
      benchmark::DoNotOptimize(s);
    } else {
      auto s = store.db->Delete(wo, ldb_slice(k));
      benchmark::DoNotOptimize(s);
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

// ──────────────────────────── Get (no block cache) ───────────────────────────
// Hypothesis: with LevelDB's block cache removed, Get latency should be close
// to ByteCask's, since both engines then go to the OS page cache per lookup.

static void LDB_Get_NoCache(benchmark::State &state) {
  LdbStore_NoCache store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  // no_block_cache on ReadOptions is redundant when block_cache=nullptr on
  // open, but set it explicitly for clarity.
  leveldb::ReadOptions ro;
  ro.fill_cache = false;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    std::string value;
    const auto t0 = std::chrono::high_resolution_clock::now();
    auto s = store.db->Get(ro, ldb_slice(k), &value);
    const auto t1 = std::chrono::high_resolution_clock::now();
    benchmark::DoNotOptimize(value);
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

// ──────────────────────────── Range (no block cache) ─────────────────────────

static void LDB_Range50_NoCache(benchmark::State &state) {
  LdbStore_NoCache store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  leveldb::ReadOptions ro;
  ro.fill_cache = false;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &start_key = keys[idx % keys.size()];
    const auto t0 = std::chrono::high_resolution_clock::now();
    std::unique_ptr<leveldb::Iterator> it{store.db->NewIterator(ro)};
    it->Seek(ldb_slice(start_key));
    int count = 0;
    for (; it->Valid() && count < kRangeLen; it->Next(), ++count) {
      benchmark::DoNotOptimize(it->value());
    }
    const auto t1 = std::chrono::high_resolution_clock::now();
    if (samples.size() < kMaxSamples)
      samples.push_back(static_cast<double>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
              .count()));
    ++idx;
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kRangeLen);
  state.counters["scans_per_us"] = benchmark::Counter(
      static_cast<double>(state.iterations()), benchmark::Counter::kIsRate);
  attach_jitter(state, samples);
}

// ──────────────────────────── Mixed (no block cache) ────────────────────────

static void LDB_Mixed_NoCache(benchmark::State &state) {
  LdbStore_NoCache store;
  auto keys = generate_prefixed_keys(kDatasetSize);
  auto val = make_value();
  leveldb::ReadOptions ro;
  ro.fill_cache = false;
  leveldb::WriteOptions wo;
  wo.sync = false;

  std::size_t idx = 0;
  std::vector<double> samples;
  samples.reserve(kMaxSamples);

  for (auto _ : state) {
    const auto &k = keys[idx % keys.size()];
    const auto op = idx % 10;
    const auto t0 = std::chrono::high_resolution_clock::now();
    if (op <= 7) {
      std::string value;
      auto s = store.db->Get(ro, ldb_slice(k), &value);
      benchmark::DoNotOptimize(value);
    } else if (op == 8) {
      auto s = store.db->Put(wo, ldb_slice(k), ldb_val_slice(val));
      benchmark::DoNotOptimize(s);
    } else {
      auto s = store.db->Delete(wo, ldb_slice(k));
      benchmark::DoNotOptimize(s);
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

// --- ByteCask ---
BENCHMARK(BC_Put_NoSync)    ->Name("ByteCask/Put/NoSync") ->Repetitions(3)->ReportAggregatesOnly(true);
BENCHMARK(BC_Put_Sync)      ->Name("ByteCask/Put/Sync")   ->Repetitions(3)->ReportAggregatesOnly(true)->Iterations(200);
BENCHMARK(BC_Get)           ->Name("ByteCask/Get");
BENCHMARK(BC_Del_NoSync)    ->Name("ByteCask/Del/NoSync") ->Repetitions(3)->ReportAggregatesOnly(true);
BENCHMARK(BC_Range50)           ->Name("ByteCask/Range50");
BENCHMARK(BC_Range1000)         ->Name("ByteCask/Range1000");
BENCHMARK(BC_Mixed_Sync)    ->Name("ByteCask/Mixed/Sync")   ->Repetitions(3)->ReportAggregatesOnly(true)->Iterations(1000);
BENCHMARK(BC_Mixed_NoSync)  ->Name("ByteCask/Mixed/NoSync");

// --- LevelDB ---
BENCHMARK(LDB_Put_NoSync)   ->Name("LevelDB/Put/NoSync")  ->Repetitions(3)->ReportAggregatesOnly(true);
BENCHMARK(LDB_Put_Sync)     ->Name("LevelDB/Put/Sync")    ->Repetitions(3)->ReportAggregatesOnly(true)->Iterations(200);
BENCHMARK(LDB_Get)          ->Name("LevelDB/Get");
BENCHMARK(LDB_Del_NoSync)   ->Name("LevelDB/Del/NoSync")  ->Repetitions(3)->ReportAggregatesOnly(true);
BENCHMARK(LDB_Range50)       ->Name("LevelDB/Range50");
BENCHMARK(LDB_Range1000)     ->Name("LevelDB/Range1000");
BENCHMARK(LDB_Mixed_Sync)   ->Name("LevelDB/Mixed/Sync")    ->Repetitions(3)->ReportAggregatesOnly(true)->Iterations(1000);
BENCHMARK(LDB_Mixed_NoSync) ->Name("LevelDB/Mixed/NoSync");

// --- LevelDB (block cache disabled) ---
BENCHMARK(LDB_Get_NoCache)        ->Name("LevelDB_NoCache/Get");
BENCHMARK(LDB_Range50_NoCache)    ->Name("LevelDB_NoCache/Range50");
BENCHMARK(LDB_Mixed_NoCache)      ->Name("LevelDB_NoCache/Mixed/NoSync");

// clang-format on

// Publish dataset_size into the JSON context section so the CSV tracking
// script can record it as a column alongside git_commit and timestamp.
// NOLINTNEXTLINE(cert-err58-cpp)
const bool kDatasetSizeContext = [] {
  benchmark::AddCustomContext("dataset_size", std::to_string(kDatasetSize));
  return true;
}();

BENCHMARK_MAIN();
