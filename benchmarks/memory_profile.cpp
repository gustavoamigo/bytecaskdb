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
#include "unordered_view.h"
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

auto use_unordered_view() -> bool {
  const char *env = std::getenv("BC_USE_UNORDERED_VIEW");
  return env && *env && std::string{env} != "0";
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

void purge_jemalloc() {
  // Force jemalloc to return all freed pages to the OS so that RSS
  // reflects live allocations, not the allocator's retained free lists.
  unsigned narenas = 0;
  std::size_t sz = sizeof(narenas);
  mallctl("arenas.narenas", &narenas, &sz, nullptr, 0);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "arena.%u.purge", narenas);
  mallctl(buf, nullptr, nullptr, nullptr, 0);
}

void print_memory(const char *phase) {
  //purge_jemalloc();
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
  auto use_uv = use_unordered_view();
  auto *shape = key_generators::key_shape_by_name(format);
  if (!shape) {
    std::fprintf(stderr, "Unknown BC_KEY_FORMAT: %s\nAvailable formats:", format.c_str());
    for (auto &s : key_generators::all_key_shapes())
      std::fprintf(stderr, " %s", s.name.c_str());
    std::fprintf(stderr, "\n");
    return 1;
  }

  // Compute avg key size from a single sample key — no bulk allocation.
  std::string key_buf;
  shape->make_key(0, n, key_buf);
  auto avg_key_size = key_buf.size();

  auto val = make_value();
  bytecask::BytesView val_view{val.data(), val.size()};

  std::printf("=== Memory Profile (%zu keys, %zu-byte %s keys, %zu-byte values%s) ===\n",
              n, avg_key_size, format.c_str(), kValueSize,
              use_uv ? ", UnorderedView" : "");

  const char *base = std::getenv("BC_BENCH_DIR");
  auto parent = base && *base ? std::filesystem::path{base}
                              : std::filesystem::path{"./.tmp"};
  auto dir = parent / "memory_profile";
  std::filesystem::create_directories(dir);

  print_memory("before open");

  {
    auto db = bytecask::DB::open(dir);

    if (use_uv) {
      // Route through UnorderedView — linear hashing maps arbitrary keys
      // into sequential bucket keys for efficient radix tree compression.
      unordered_view::Options uv_opts;
      const char *cap_env = std::getenv("BC_UV_BUCKET_CAPACITY");
      if (cap_env && *cap_env)
        uv_opts.bucket_capacity = static_cast<std::uint32_t>(std::stoul(cap_env));
      unordered_view::UnorderedView view{db, "uv", uv_opts};
      for (std::size_t i = 0; i < n; ++i) {
        shape->make_key(i, n, key_buf);
        view.put(bc_key(key_buf), val_view);
      }
      // Final sync.
      shape->make_key(0, n, key_buf);
      db.put({.sync = true}, bc_key(key_buf), val_view);

      // Print allocation / I/O counters.
      auto &s = view.stats();
      std::printf("\n  UnorderedView stats:\n");
      std::printf("    splits:              %12lu  (empty: %lu)\n", s.splits, s.split_empty);
      std::printf("    split_entries_moved:  %12lu\n", s.split_entries_moved);
      std::printf("    split_tombstones_gc: %12lu\n", s.split_tombstones_gc);
      std::printf("    split_db_reads:      %12lu\n", s.split_db_reads);
      std::printf("    split_db_writes:     %12lu\n", s.split_db_writes);
      std::printf("    put_append:          %12lu  (no-read fast path)\n", s.put_append);
      std::printf("    put_bloom_skip:      %12lu  (bloom no-collision → overwrite)\n", s.put_bloom_skip);
      std::printf("    put_bloom_rmw:       %12lu  (bloom maybe-collision → RMW)\n", s.put_bloom_rmw);
      std::printf("    bloom_collisions_set:%12lu\n", s.bloom_collisions_set);
      std::printf("    put_chain_update:    %12lu  (read-modify-write)\n", s.put_chain_update);
      std::printf("    chain_decodes:       %12lu\n", s.chain_decodes);
      std::printf("    chain_encodes:       %12lu\n", s.chain_encodes);
      std::printf("    splits/put ratio:    %12.4f\n",
                  n > 0 ? static_cast<double>(s.splits) / static_cast<double>(n) : 0.0);
      std::printf("    entries_moved/put:   %12.4f\n",
                  n > 0 ? static_cast<double>(s.split_entries_moved) / static_cast<double>(n) : 0.0);
      std::printf("\n");
    } else {
      bytecask::WriteOptions wo;
      wo.sync = false;

      for (std::size_t i = 0; i < n; i += kPopulateBatchSize) {
        auto end = std::min(i + kPopulateBatchSize, n);
        bytecask::WritePlan plan;
        for (std::size_t j = i; j < end; ++j) {
          shape->make_key(j, n, key_buf);
          plan.put(bc_key(key_buf), val_view);
        }
        wo.sync = (end == n);
        (void)db.apply_batch(wo, std::move(plan));
      }
    }
    //(void)db.vacuum();
    (void)db.flush_hints();

    print_memory("after insert");

    // Dump all DB keys to a file for inspection.
    const char *dump_env = std::getenv("BC_DUMP_KEYS");
    if (dump_env && *dump_env) {
      auto *f = std::fopen(dump_env, "w");
      if (f) {
        std::size_t count = 0;
        for (auto &key : db.keys_from({}, {})) {
          // Print each key as hex + ASCII side-by-side.
          for (auto b : key)
            std::fprintf(f, "%02x", std::to_integer<unsigned>(b));
          std::fprintf(f, "  ");
          for (auto b : key) {
            auto c = std::to_integer<unsigned char>(b);
            std::fprintf(f, "%c", (c >= 32 && c < 127) ? c : '.');
          }
          std::fprintf(f, "\n");
          ++count;
        }
        std::fclose(f);
        std::printf("  Dumped %zu keys to %s\n", count, dump_env);
      }
    }
  }

  // purge_jemalloc();
  print_memory("after close");

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);

  return 0;
}
