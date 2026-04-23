// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — memory profile: measures heap usage with N keys loaded.
//
// Usage:
//   BC_DATASET_SIZE=1000000 ./memory_profile
//   BC_DATASET_SIZE=1000000 node build/memory_profile.js  (WASM)

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <mimalloc.h>
#else
#include <sys/resource.h>
#endif

import bytecask;

namespace {

constexpr std::size_t kValueSize = 245;
constexpr std::size_t kPopulateBatchSize = 100;

auto dataset_size() -> std::size_t {
  const char *env = std::getenv("BC_DATASET_SIZE");
  if (env && *env)
    return static_cast<std::size_t>(std::stoul(env));
  return 50'000;
}

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

void print_mib(const char *label, std::size_t bytes) {
  std::printf("  %-18s %8.1f MiB  (%zu bytes)\n", label,
              static_cast<double>(bytes) / (1024.0 * 1024.0), bytes);
}

#ifdef __EMSCRIPTEN__
auto measure_heap_allocated() -> std::size_t {
  std::size_t total = 0;
  mi_heap_visit_blocks(
      mi_heap_get_default(), false,
      [](const mi_heap_t *, const mi_heap_area_t *area, void *, size_t,
         void *arg) -> bool {
        *static_cast<std::size_t *>(arg) += area->used * area->block_size;
        return true;
      },
      &total);
  return total;
}

auto measure_wasm_memory() -> std::size_t {
  return static_cast<std::size_t>(__builtin_wasm_memory_size(0)) * 65536;
}

void print_memory(const char *phase) {
  std::printf("  [%s]\n", phase);
  print_mib("Heap allocated:", measure_heap_allocated());
  print_mib("WASM memory:", measure_wasm_memory());
}
#else
void print_memory(const char *phase) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  std::printf("  [%s]\n", phase);
  print_mib("Peak RSS:", static_cast<std::size_t>(ru.ru_maxrss) * 1024);
}
#endif

} // namespace

int main() {
  auto n = dataset_size();
  auto keys = generate_prefixed_keys(n);
  auto val = make_value();

  const char *base = std::getenv("BC_BENCH_DIR");
  auto parent = base && *base ? std::filesystem::path{base}
                              : std::filesystem::temp_directory_path();
  auto dir = parent / "memory_profile";
  std::filesystem::create_directories(dir);

  std::printf("=== Memory Profile (%zu keys, %zu-byte values) ===\n", n,
              kValueSize);

  print_memory("before open");

  {
    auto db = bytecask::DB::open(dir);
    bytecask::WriteOptions wo;
    wo.sync = false;

    for (std::size_t i = 0; i < keys.size(); i += kPopulateBatchSize) {
      auto end = std::min(i + kPopulateBatchSize, keys.size());
      bytecask::WritePlan plan;
      for (std::size_t j = i; j < end; ++j) {
        plan.put(bc_key(keys[j]), bc_val(val));
      }
      wo.sync = (end == keys.size());
      (void)db.apply_batch(wo, std::move(plan));
    }

    print_memory("after insert");
  }

  print_memory("after close");

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  return 0;
}
