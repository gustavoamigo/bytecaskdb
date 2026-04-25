// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — memory profile: measures heap usage with N keys loaded.
//
// Usage:
//   BC_DATASET_SIZE=1000000 ./memory_profile
//   BC_KEY_FORMAT=sha256_hex BC_DATASET_SIZE=10000 ./memory_profile
//   BC_DATASET_SIZE=1000000 node build/memory_profile.js  (WASM)
//
// Available key formats (BC_KEY_FORMAT):
//   prefixed (default), uniform, short, incremental, uuidv7, uuidv7_binary,
//   sha256_hex, sha256_bin, uuidv4_text, uuidv4_prefixed, uuidv4_binary,
//   hash_prefixed, binary, zipfian, clustered, many_partitions, mixed

#include "../tests/key_generators.h"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <mimalloc.h>
#else
#include <jemalloc/jemalloc.h>
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

auto key_format() -> std::string {
  const char *env = std::getenv("BC_KEY_FORMAT");
  return (env && *env) ? std::string{env} : "prefixed";
}

auto make_value() -> std::array<std::byte, kValueSize> {
  std::array<std::byte, kValueSize> v{};
  for (std::size_t i = 0; i < kValueSize; ++i)
    v[i] = static_cast<std::byte>(i & 0xFF);
  return v;
}

auto bc_key(const std::string &s) -> bytecask::BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
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
auto measure_current_rss() -> std::size_t {
  std::FILE *f = std::fopen("/proc/self/statm", "r");
  if (!f)
    return 0;
  unsigned long pages = 0;
  unsigned long resident = 0;
  // statm fields: size resident shared text lib data dt
  if (std::fscanf(f, "%lu %lu", &pages, &resident) != 2)
    resident = 0;
  std::fclose(f);
  return static_cast<std::size_t>(resident) * 4096;
}

void print_memory(const char *phase) {
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  std::printf("  [%s]\n", phase);
  print_mib("RSS:", measure_current_rss());
  print_mib("Peak RSS:", static_cast<std::size_t>(ru.ru_maxrss) * 1024);
}
#endif

} // namespace

int main() {
  auto n = dataset_size();
  auto format = key_format();
  auto *shape = key_generators::key_shape_by_name(format);
  if (!shape) {
    std::fprintf(stderr, "Unknown BC_KEY_FORMAT: %s\nAvailable formats:", format.c_str());
    for (auto &s : key_generators::all_key_shapes())
      std::fprintf(stderr, " %s", s.name.c_str());
    std::fprintf(stderr, "\n");
    return 1;
  }

  auto keys = shape->generate(n);
  auto val = make_value();
  bytecask::BytesView val_view{val.data(), val.size()};

  std::size_t total_key_bytes = 0;
  for (auto &k : keys)
    total_key_bytes += k.size();
  auto avg_key_size = keys.empty() ? std::size_t{0} : total_key_bytes / keys.size();
  std::printf("=== Memory Profile (%zu keys, %zu-byte %s keys, %zu-byte values) ===\n",
              n, avg_key_size, format.c_str(), kValueSize);

  const char *base = std::getenv("BC_BENCH_DIR");
  auto parent = base && *base ? std::filesystem::path{base}
                              : std::filesystem::path{"./.tmp"};
  auto dir = parent / "memory_profile";
  std::filesystem::create_directories(dir);

  print_memory("before open");

  {
    // Use large file rotation threshold to avoid hitting fd limits at scale.
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 512 * 1024 * 1024});
    bytecask::WriteOptions wo;
    wo.sync = false;

    for (std::size_t i = 0; i < n; i += kPopulateBatchSize) {
      auto end = std::min(i + kPopulateBatchSize, n);
      bytecask::WritePlan plan;
      for (std::size_t j = i; j < end; ++j)
        plan.put(bc_key(keys[j]), val_view);
      wo.sync = (end == n);
      (void)db.apply_batch(wo, std::move(plan));
    }

    print_memory("after insert");
  }

  print_memory("after close");

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  return 0;
}
