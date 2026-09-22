// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — write amplification probe for the append path.
//
// Measures, in isolation from MariaDB/sysbench/the key directory, how many
// bytes reach the block layer for each byte a caller appends. Appends records
// of a chosen size to a WritablePosixDataFile on a chosen sync cadence, and
// compares the payload handed to the engine against /proc/diskstats for the
// device backing the file.
//
// Hidden by default: it reports numbers rather than asserting a target, needs
// a real block device, and is perturbed by any other writer on that device.
//   xmake run bytecask_tests "[write_amp]"
//   BYTECASK_PROBE_DIR=/mnt/nvme xmake run bytecask_tests "[write_amp]"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/sysmacros.h>

import bytecask.data_file;
import bytecask.types;

namespace {

using namespace bytecask;

// Sectors written to the device backing *path*, in bytes. /proc/diskstats
// field 10 is "sectors written"; sectors are 512 bytes there regardless of the
// device's logical block size.
// nullopt means the path is not backed by a block device at all (tmpfs, for
// instance — /tmp is tmpfs on many systems, and returning 0 there would make
// the probe silently report no I/O rather than admit it cannot measure).
[[nodiscard]] auto device_written_bytes(const std::filesystem::path& path)
    -> std::optional<std::uint64_t> {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return std::nullopt;
  const auto want_major = ::major(st.st_dev);
  const auto want_minor = ::minor(st.st_dev);

  std::ifstream f{"/proc/diskstats"};
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream is{line};
    unsigned maj = 0, min_ = 0;
    std::string name;
    if (!(is >> maj >> min_ >> name)) continue;
    if (maj != want_major || min_ != want_minor) continue;
    std::uint64_t v = 0;
    for (int i = 0; i < 6; ++i) {  // fields 4..9
      if (!(is >> v)) return std::nullopt;
    }
    if (!(is >> v)) return std::nullopt;  // field 10: sectors written
    return v * 512ULL;
  }
  return std::nullopt;
}

struct Result {
  std::uint64_t payload_bytes;
  std::uint64_t device_bytes;
  std::uint64_t file_bytes;
};

// Appends *count* records of *value_size*, calling sync() every *sync_every*
// appends. capacity == 0 disables zero-fill-ahead.
[[nodiscard]] auto measure(const std::filesystem::path& path, std::size_t count,
                           std::size_t value_size, std::size_t sync_every,
                           std::size_t capacity) -> Result {
  std::error_code ec;
  std::filesystem::remove(path, ec);

  const std::vector<std::byte> key(16, std::byte{0xAB});
  const std::vector<std::byte> value(value_size, std::byte{0xCD});

  const auto dev_before = device_written_bytes(path.parent_path()).value_or(0);
  std::uint64_t payload = 0;
  {
    auto file = WritablePosixDataFile::create(path, capacity);
    for (std::size_t i = 0; i < count; ++i) {
      (void)file->append_entry(i + 1, EntryType::Put, key, value);
      payload += key.size() + value.size();
      if (sync_every != 0 && (i + 1) % sync_every == 0) file->sync();
    }
    file->sync();
  }
  const auto dev_after = device_written_bytes(path.parent_path()).value_or(0);
  const auto size = std::filesystem::file_size(path, ec);

  std::filesystem::remove(path, ec);
  return {payload, dev_after - dev_before, ec ? 0 : size};
}

}  // namespace

TEST_CASE("append path write amplification", "[.write_amp]") {
  const char* env = std::getenv("BYTECASK_PROBE_DIR");
  // Not temp_directory_path(): /tmp is tmpfs on many systems and nothing there
  // reaches a block device, so the probe would report zeros that look like a
  // result. The working directory is at least on real storage.
  const auto dir = env ? std::filesystem::path{env}
                       : std::filesystem::current_path();
  const auto path = dir / "bytecask_write_amp_probe.data";

  if (!device_written_bytes(dir).has_value()) {
    SKIP("no block device backs " + dir.string() +
         " (tmpfs?) — set BYTECASK_PROBE_DIR to a directory on real storage");
  }

  constexpr std::size_t kCount = 20000;
  constexpr std::size_t kCapacity = 256uz * 1024 * 1024;

  std::printf("\nprobe dir: %s   (%zu records per case)\n", dir.c_str(), kCount);
  std::printf("%10s %10s %10s %12s %12s %12s %8s\n", "value_B", "sync_every",
              "zero_fill", "payloadMiB", "deviceMiB", "fileMiB", "amp");

  for (const std::size_t value_size : {64uz, 1024uz, 4096uz}) {
    for (const std::size_t sync_every : {1uz, 16uz}) {
      for (const bool zero_fill : {true, false}) {
        const auto r = measure(path, kCount, value_size, sync_every,
                               zero_fill ? kCapacity : 0);
        constexpr double kMiB = 1024.0 * 1024.0;
        std::printf("%10zu %10zu %10s %12.1f %12.1f %12.1f %8.2f\n", value_size,
                    sync_every, zero_fill ? "on" : "off",
                    static_cast<double>(r.payload_bytes) / kMiB,
                    static_cast<double>(r.device_bytes) / kMiB,
                    static_cast<double>(r.file_bytes) / kMiB,
                    r.payload_bytes
                        ? static_cast<double>(r.device_bytes) /
                              static_cast<double>(r.payload_bytes)
                        : 0.0);
        CHECK(r.payload_bytes > 0);
      }
    }
  }
}
