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

static constexpr std::array kPrefixes = {
    "user::", "order::", "session::", "invoice::", "product::"};

// Binary prefix bytes: 1-byte tag per bucket (0x01..0x05).
static constexpr std::array<std::byte, 1> kBinaryPrefixes[] = {
    {std::byte{0x01}}, {std::byte{0x02}}, {std::byte{0x03}},
    {std::byte{0x04}}, {std::byte{0x05}}};

auto dataset_size() -> std::size_t {
  const char *env = std::getenv("BC_DATASET_SIZE");
  if (env && *env)
    return static_cast<std::size_t>(std::stoul(env));
  return 50'000;
}

auto use_binary_keys() -> bool {
  const char *env = std::getenv("BC_KEY_FORMAT");
  return env && std::string_view{env} == "binary";
}

// Generates a text key: prefix + UUIDv7-style string.
auto make_text_key(std::size_t i, std::size_t per_prefix) -> std::string {
  auto prefix_idx = i / per_prefix;
  auto local_idx = i % per_prefix;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s018f6e2c-%04lx-7000-8000-%012lx",
                kPrefixes[prefix_idx],
                static_cast<unsigned long>(local_idx >> 16),
                static_cast<unsigned long>(local_idx & 0xFFFFFFFFULL));
  return std::string{buf};
}

// Generates a binary key: 1-byte prefix tag + 16-byte UUIDv7.
auto make_binary_key(std::size_t i, std::size_t per_prefix)
    -> std::vector<std::byte> {
  auto prefix_idx = i / per_prefix;
  auto local_idx = i % per_prefix;
  // 1-byte prefix + 16-byte UUID = 17 bytes total.
  std::vector<std::byte> key(17);
  key[0] = kBinaryPrefixes[prefix_idx][0];
  // UUIDv7: first 6 bytes timestamp-like, then version/variant, then random.
  // We use a deterministic layout matching the text key's structure.
  key[1] = std::byte{0x01};
  key[2] = std::byte{0x8f};
  key[3] = std::byte{0x6e};
  key[4] = std::byte{0x2c};
  key[5] = static_cast<std::byte>((local_idx >> 24) & 0xFF);
  key[6] = static_cast<std::byte>((local_idx >> 16) & 0xFF);
  key[7] = std::byte{0x70};  // version nibble
  key[8] = std::byte{0x00};
  key[9] = std::byte{0x80};  // variant bits
  key[10] = std::byte{0x00};
  key[11] = static_cast<std::byte>((local_idx >> 24) & 0xFF);
  key[12] = static_cast<std::byte>((local_idx >> 16) & 0xFF);
  key[13] = static_cast<std::byte>((local_idx >> 8) & 0xFF);
  key[14] = static_cast<std::byte>(local_idx & 0xFF);
  key[15] = std::byte{0x00};
  key[16] = std::byte{0x00};
  return key;
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
  auto binary = use_binary_keys();
  auto val = make_value();
  bytecask::BytesView val_view{val.data(), val.size()};

  const char *base = std::getenv("BC_BENCH_DIR");
  auto parent = base && *base ? std::filesystem::path{base}
                              : std::filesystem::path{"./.tmp"};
  auto dir = parent / "memory_profile";
  std::filesystem::create_directories(dir);

  std::size_t key_size = binary
      ? make_binary_key(0, per_prefix).size()
      : make_text_key(0, per_prefix).size();
  const char *format_label = binary ? "binary" : "text";
  std::printf("=== Memory Profile (%zu keys, %zu-byte %s keys, %zu-byte values) ===\n",
              n, key_size, format_label, kValueSize);

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
        if (binary) {
          auto key = make_binary_key(j, per_prefix);
          plan.put(bytecask::BytesView{key.data(), key.size()}, val_view);
        } else {
          auto key = make_text_key(j, per_prefix);
          plan.put(bc_key(key), val_view);
        }
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
