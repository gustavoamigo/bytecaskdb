// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — memory profile: measures heap usage with N keys loaded.
//
// Usage:
//   BC_DATASET_SIZE=1000000 ./memory_profile
//   BC_DATASET_SIZE=1000000 node build/memory_profile.js  (WASM)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>

#ifdef __EMSCRIPTEN__
#include <mimalloc.h>
#else
#include <mimalloc.h>
#include <sys/resource.h>
#endif

import bytecask;

namespace {

constexpr std::size_t kValueSize = 245;
constexpr std::size_t kPopulateBatchSize = 100;

static constexpr std::array kPrefixes = {
    "user::", "order::", "session::", "invoice::", "product::"};

auto dataset_size() -> std::size_t {
  const char *env = std::getenv("BC_DATASET_SIZE");
  if (env && *env)
    return static_cast<std::size_t>(std::stoul(env));
  return 50'000;
}

// Generates key i on demand — no pre-allocated vector needed.
auto make_key(std::size_t i, std::size_t per_prefix) -> std::string {
  auto prefix_idx = i / per_prefix;
  auto local_idx = i % per_prefix;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s018f6e2c-%04lx-7000-8000-%012lx",
                kPrefixes[prefix_idx],
                static_cast<unsigned long>(local_idx >> 16),
                static_cast<unsigned long>(local_idx & 0xFFFFFFFFULL));
  return std::string{buf};
}

auto make_value() -> std::array<std::byte, kValueSize> {
  std::array<std::byte, kValueSize> v{};
  // Deterministic fill — content doesn't matter for memory profiling.
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
  auto per_prefix = n / kPrefixes.size();
  auto val = make_value();
  bytecask::BytesView val_view{val.data(), val.size()};

  const char *base = std::getenv("BC_BENCH_DIR");
  auto parent = base && *base ? std::filesystem::path{base}
                              : std::filesystem::temp_directory_path();
  auto dir = parent / "memory_profile";
  std::filesystem::create_directories(dir);
  
  auto sample_key = make_key(0, per_prefix);
  std::printf("=== Memory Profile (%zu keys, %zu-byte keys, %zu-byte values) ===\n",
              n, sample_key.size(), kValueSize);

  print_memory("before open");

  {
    // Use large file rotation threshold to avoid hitting fd limits at scale.
    auto db = bytecask::DB::open(dir, {.max_file_bytes = 512 * 1024 * 1024});
    bytecask::WriteOptions wo;
    wo.sync = false;

    for (std::size_t i = 0; i < n; i += kPopulateBatchSize) {
      auto end = std::min(i + kPopulateBatchSize, n);
      bytecask::WritePlan plan;
      for (std::size_t j = i; j < end; ++j) {
        auto key = make_key(j, per_prefix);
        plan.put(bc_key(key), val_view);
      }
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
