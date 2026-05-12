#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

static constexpr int kNumRecords = 2000;
static constexpr int kRecordSize = 2048;
static constexpr int kSyncInterval = 10;
static constexpr int kWarmupRecords = 1000;

struct BenchResult {
  double elapsed_ms;
  double ops_per_sec;
  double mb_per_sec;
};

static auto generate_records(int n, int size) -> std::vector<std::vector<char>> {
  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 255);
  std::vector<std::vector<char>> records(n);
  for (auto& rec : records) {
    rec.resize(size);
    for (auto& b : rec) b = static_cast<char>(dist(rng));
  }
  return records;
}

// --- writev benchmarks ---

static auto bench_writev_every_sync(const std::vector<std::vector<char>>& records,
                                    const std::string& path) -> BenchResult {
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  // Warmup
  for (int i = 0; i < kWarmupRecords; ++i) {
    iovec iov{.iov_base = const_cast<char*>(records[i].data()),
              .iov_len = records[i].size()};
    writev(fd, &iov, 1);
    fdatasync(fd);
  }
  lseek(fd, 0, SEEK_SET);

  auto start = std::chrono::steady_clock::now();
  for (auto& rec : records) {
    iovec iov{.iov_base = const_cast<char*>(rec.data()),
              .iov_len = rec.size()};
    writev(fd, &iov, 1);
    fdatasync(fd);
  }
  auto end = std::chrono::steady_clock::now();
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

static auto bench_writev_batch_sync(const std::vector<std::vector<char>>& records,
                                    const std::string& path) -> BenchResult {
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  // Warmup
  for (int i = 0; i < kWarmupRecords; ++i) {
    iovec iov{.iov_base = const_cast<char*>(records[i].data()),
              .iov_len = records[i].size()};
    writev(fd, &iov, 1);
    if ((i + 1) % kSyncInterval == 0) fdatasync(fd);
  }
  lseek(fd, 0, SEEK_SET);

  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < static_cast<int>(records.size()); ++i) {
    iovec iov{.iov_base = const_cast<char*>(records[i].data()),
              .iov_len = records[i].size()};
    writev(fd, &iov, 1);
    if ((i + 1) % kSyncInterval == 0) fdatasync(fd);
  }
  // Final sync for trailing records
  if (records.size() % kSyncInterval != 0) fdatasync(fd);
  auto end = std::chrono::steady_clock::now();
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// writev with O_DSYNC — kernel syncs data on every write, no separate fdatasync call
static auto bench_writev_odsync(const std::vector<std::vector<char>>& records,
                                const std::string& path) -> BenchResult {
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DSYNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  for (int i = 0; i < kWarmupRecords; ++i) {
    iovec iov{.iov_base = const_cast<char*>(records[i].data()),
              .iov_len = records[i].size()};
    writev(fd, &iov, 1);
  }
  lseek(fd, 0, SEEK_SET);

  auto start = std::chrono::steady_clock::now();
  for (auto& rec : records) {
    iovec iov{.iov_base = const_cast<char*>(rec.data()),
              .iov_len = rec.size()};
    writev(fd, &iov, 1);
  }
  auto end = std::chrono::steady_clock::now();
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// O_DIRECT | O_DSYNC — bypass page cache, kernel syncs on every write
static constexpr size_t kAlignment = 4096;

static auto bench_writev_direct_dsync(const std::vector<std::vector<char>>& records,
                                      const std::string& path) -> BenchResult {
  //static_assert(kRecordSize % 512 == 0, "O_DIRECT requires size aligned to sector");
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | O_DSYNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  void* aligned_buf = nullptr;
  posix_memalign(&aligned_buf, kAlignment, kRecordSize);

  // Warmup
  for (int i = 0; i < kWarmupRecords; ++i) {
    std::memcpy(aligned_buf, records[i].data(), kRecordSize);
    iovec iov{.iov_base = aligned_buf, .iov_len = kRecordSize};
    writev(fd, &iov, 1);
  }
  lseek(fd, 0, SEEK_SET);

  auto start = std::chrono::steady_clock::now();
  for (auto& rec : records) {
    std::memcpy(aligned_buf, rec.data(), kRecordSize);
    iovec iov{.iov_base = aligned_buf, .iov_len = kRecordSize};
    writev(fd, &iov, 1);
  }
  auto end = std::chrono::steady_clock::now();
  free(aligned_buf);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// pwritev2 with RWF_DSYNC — per-call sync flag, avoids O_DSYNC on the fd
static auto bench_pwritev2_rwf_dsync(const std::vector<std::vector<char>>& records,
                                     const std::string& path) -> BenchResult {
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  off_t offset = 0;
  for (int i = 0; i < kWarmupRecords; ++i) {
    iovec iov{.iov_base = const_cast<char*>(records[i].data()),
              .iov_len = records[i].size()};
    pwritev2(fd, &iov, 1, offset, RWF_DSYNC);
    offset += static_cast<off_t>(records[i].size());
  }

  offset = 0;
  auto start = std::chrono::steady_clock::now();
  for (auto& rec : records) {
    iovec iov{.iov_base = const_cast<char*>(rec.data()),
              .iov_len = rec.size()};
    pwritev2(fd, &iov, 1, offset, RWF_DSYNC);
    offset += static_cast<off_t>(rec.size());
  }
  auto end = std::chrono::steady_clock::now();
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// O_DIRECT (no O_DSYNC): N separate writes, one fdatasync at batch boundary.
// Writes bypass page cache; sync is amortized across the batch.
static auto bench_writev_direct_batch_fdatasync(const std::vector<std::vector<char>>& records,
                                                const std::string& path) -> BenchResult {
  //static_assert(kRecordSize % 512 == 0, "O_DIRECT requires size aligned to sector");
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  void* aligned_buf = nullptr;
  posix_memalign(&aligned_buf, kAlignment, kRecordSize);

  // Warmup
  for (int i = 0; i < kWarmupRecords; ++i) {
    std::memcpy(aligned_buf, records[i].data(), kRecordSize);
    iovec iov{.iov_base = aligned_buf, .iov_len = kRecordSize};
    writev(fd, &iov, 1);
    if ((i + 1) % kSyncInterval == 0) fdatasync(fd);
  }
  lseek(fd, 0, SEEK_SET);

  int n = static_cast<int>(records.size());
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    std::memcpy(aligned_buf, records[i].data(), kRecordSize);
    iovec iov{.iov_base = aligned_buf, .iov_len = kRecordSize};
    writev(fd, &iov, 1);
    if ((i + 1) % kSyncInterval == 0) fdatasync(fd);
  }
  if (n % kSyncInterval != 0) fdatasync(fd);
  auto end = std::chrono::steady_clock::now();
  free(aligned_buf);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// O_DIRECT | O_DSYNC, page-sync style: rewrite the same block on each new entry,
// padded with prior entries + zeros. Each writev is durable on return — same
// contract as fdatasync-per-write, no separate sync syscall. Mirrors how mmap
// dirties and flushes a whole page at a time.
static auto bench_writev_direct_dsync_block(const std::vector<std::vector<char>>& records,
                                            const std::string& path) -> BenchResult {
 // static_assert(kRecordSize % 512 == 0, "O_DIRECT requires size aligned to sector");
  constexpr size_t kBlockSize = 4096;
  //static_assert(kBlockSize % kRecordSize == 0, "block must hold whole records");
  constexpr int kEntriesPerBlock = kBlockSize / kRecordSize;

  auto n = static_cast<int>(records.size());
  size_t blocks = (static_cast<size_t>(n) + kEntriesPerBlock - 1) / kEntriesPerBlock;
  auto total = static_cast<off_t>(blocks * kBlockSize);

  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | O_DSYNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  void* block_buf = nullptr;
  posix_memalign(&block_buf, kAlignment, kBlockSize);
  std::memset(block_buf, 0, kBlockSize);
  auto* block = static_cast<char*>(block_buf);

  // Warmup
  for (int i = 0; i < kWarmupRecords; ++i) {
    int slot = i % kEntriesPerBlock;
    off_t block_off = static_cast<off_t>(i / kEntriesPerBlock) * kBlockSize;
    std::memcpy(block + slot * kRecordSize, records[i].data(), kRecordSize);
    iovec iov{.iov_base = block, .iov_len = kBlockSize};
    pwritev(fd, &iov, 1, block_off);
    if (slot == kEntriesPerBlock - 1) std::memset(block, 0, kBlockSize);
  }
  std::memset(block, 0, kBlockSize);

  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    int slot = i % kEntriesPerBlock;
    off_t block_off = static_cast<off_t>(i / kEntriesPerBlock) * kBlockSize;
    std::memcpy(block + slot * kRecordSize, records[i].data(), kRecordSize);
    iovec iov{.iov_base = block, .iov_len = kBlockSize};
    pwritev(fd, &iov, 1, block_off);
    if (slot == kEntriesPerBlock - 1) std::memset(block, 0, kBlockSize);
  }
  auto end = std::chrono::steady_clock::now();
  free(block_buf);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(n) / (ms / 1000.0);
  double mb = (static_cast<double>(n) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// O_DIRECT | O_DSYNC batched: one writev of kSyncInterval records per syscall.
// Bypasses page cache; O_DSYNC makes the (single) syscall durable.
static auto bench_writev_direct_dsync_batched(const std::vector<std::vector<char>>& records,
                                              const std::string& path) -> BenchResult {
  //static_assert(kRecordSize % 512 == 0, "O_DIRECT requires size aligned to sector");
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | O_DSYNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  void* aligned_buf = nullptr;
  posix_memalign(&aligned_buf, kAlignment, static_cast<size_t>(kSyncInterval) * kRecordSize);
  auto* buf = static_cast<char*>(aligned_buf);

  // Warmup
  for (int i = 0; i < kWarmupRecords; i += kSyncInterval) {
    int batch = std::min(kSyncInterval, kWarmupRecords - i);
    for (int j = 0; j < batch; ++j) {
      std::memcpy(buf + j * kRecordSize, records[i + j].data(), kRecordSize);
    }
    iovec iov{.iov_base = buf, .iov_len = static_cast<size_t>(batch) * kRecordSize};
    writev(fd, &iov, 1);
  }
  lseek(fd, 0, SEEK_SET);

  int n = static_cast<int>(records.size());
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < n; i += kSyncInterval) {
    int batch = std::min(kSyncInterval, n - i);
    for (int j = 0; j < batch; ++j) {
      std::memcpy(buf + j * kRecordSize, records[i + j].data(), kRecordSize);
    }
    iovec iov{.iov_base = buf, .iov_len = static_cast<size_t>(batch) * kRecordSize};
    writev(fd, &iov, 1);
  }
  auto end = std::chrono::steady_clock::now();
  free(aligned_buf);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// Batched writev: accumulate kSyncInterval records into a single writev + single fdatasync
static auto bench_writev_batched_io(const std::vector<std::vector<char>>& records,
                                    const std::string& path) -> BenchResult {
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  // Warmup
  std::vector<iovec> iovs(kSyncInterval);
  for (int i = 0; i < kWarmupRecords; i += kSyncInterval) {
    int batch = std::min(kSyncInterval, kWarmupRecords - i);
    for (int j = 0; j < batch; ++j) {
      iovs[j] = {.iov_base = const_cast<char*>(records[i + j].data()),
                  .iov_len = records[i + j].size()};
    }
    writev(fd, iovs.data(), batch);
    fdatasync(fd);
  }
  lseek(fd, 0, SEEK_SET);

  int n = static_cast<int>(records.size());
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < n; i += kSyncInterval) {
    int batch = std::min(kSyncInterval, n - i);
    for (int j = 0; j < batch; ++j) {
      iovs[j] = {.iov_base = const_cast<char*>(records[i + j].data()),
                  .iov_len = records[i + j].size()};
    }
    writev(fd, iovs.data(), batch);
    fdatasync(fd);
  }
  auto end = std::chrono::steady_clock::now();
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// --- mmap benchmarks ---

static auto bench_mmap_every_sync(const std::vector<std::vector<char>>& records,
                                  const std::string& path) -> BenchResult {
  size_t total = records.size() * kRecordSize;
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ftruncate(fd, static_cast<off_t>(total));

  auto* map = static_cast<char*>(mmap(nullptr, total, PROT_WRITE, MAP_SHARED, fd, 0));

  // Warmup
  for (int i = 0; i < kWarmupRecords; ++i) {
    std::memcpy(map + static_cast<size_t>(i) * kRecordSize, records[i].data(), kRecordSize);
    msync(map + static_cast<size_t>(i) * kRecordSize, kRecordSize, MS_SYNC);
  }
  munmap(map, total);
  ftruncate(fd, 0);
  ftruncate(fd, static_cast<off_t>(total));
  map = static_cast<char*>(mmap(nullptr, total, PROT_WRITE, MAP_SHARED, fd, 0));

  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < records.size(); ++i) {
    std::memcpy(map + i * kRecordSize, records[i].data(), kRecordSize);
    msync(map + i * kRecordSize, kRecordSize, MS_SYNC);
  }
  auto end = std::chrono::steady_clock::now();
  munmap(map, total);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

static auto bench_mmap_batch_sync(const std::vector<std::vector<char>>& records,
                                  const std::string& path) -> BenchResult {
  size_t total = records.size() * kRecordSize;
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ftruncate(fd, static_cast<off_t>(total));

  auto* map = static_cast<char*>(mmap(nullptr, total, PROT_WRITE, MAP_SHARED, fd, 0));

  // Warmup
  for (int i = 0; i < kWarmupRecords; ++i) {
    std::memcpy(map + static_cast<size_t>(i) * kRecordSize, records[i].data(), kRecordSize);
    if ((i + 1) % kSyncInterval == 0)
      msync(map + static_cast<size_t>(i - kSyncInterval + 1) * kRecordSize,
            static_cast<size_t>(kSyncInterval) * kRecordSize, MS_SYNC);
  }
  munmap(map, total);
  ftruncate(fd, 0);
  ftruncate(fd, static_cast<off_t>(total));
  map = static_cast<char*>(mmap(nullptr, total, PROT_WRITE, MAP_SHARED, fd, 0));

  int n = static_cast<int>(records.size());
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    std::memcpy(map + static_cast<size_t>(i) * kRecordSize, records[i].data(), kRecordSize);
    if ((i + 1) % kSyncInterval == 0) {
      msync(map + static_cast<size_t>(i - kSyncInterval + 1) * kRecordSize,
            static_cast<size_t>(kSyncInterval) * kRecordSize, MS_SYNC);
    }
  }
  // Final sync for trailing records
  if (n % kSyncInterval != 0) {
    int tail_start = (n / kSyncInterval) * kSyncInterval;
    msync(map + static_cast<size_t>(tail_start) * kRecordSize,
          static_cast<size_t>(n - tail_start) * kRecordSize, MS_SYNC);
  }
  auto end = std::chrono::steady_clock::now();
  munmap(map, total);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// mmap with fdatasync instead of msync — write to mapping, sync via fd
static auto bench_mmap_fdatasync_every(const std::vector<std::vector<char>>& records,
                                       const std::string& path) -> BenchResult {
  size_t total = records.size() * kRecordSize;
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ftruncate(fd, static_cast<off_t>(total));

  auto* map = static_cast<char*>(mmap(nullptr, total, PROT_WRITE, MAP_SHARED, fd, 0));

  for (int i = 0; i < kWarmupRecords; ++i) {
    std::memcpy(map + static_cast<size_t>(i) * kRecordSize, records[i].data(), kRecordSize);
    fdatasync(fd);
  }
  munmap(map, total);
  ftruncate(fd, 0);
  ftruncate(fd, static_cast<off_t>(total));
  map = static_cast<char*>(mmap(nullptr, total, PROT_WRITE, MAP_SHARED, fd, 0));

  auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < records.size(); ++i) {
    std::memcpy(map + i * kRecordSize, records[i].data(), kRecordSize);
    fdatasync(fd);
  }
  auto end = std::chrono::steady_clock::now();
  munmap(map, total);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

static auto bench_mmap_fdatasync_batch(const std::vector<std::vector<char>>& records,
                                       const std::string& path) -> BenchResult {
  size_t total = records.size() * kRecordSize;
  int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ftruncate(fd, static_cast<off_t>(total));

  auto* map = static_cast<char*>(mmap(nullptr, total, PROT_WRITE, MAP_SHARED, fd, 0));

  for (int i = 0; i < kWarmupRecords; ++i) {
    std::memcpy(map + static_cast<size_t>(i) * kRecordSize, records[i].data(), kRecordSize);
    if ((i + 1) % kSyncInterval == 0) fdatasync(fd);
  }
  munmap(map, total);
  ftruncate(fd, 0);
  ftruncate(fd, static_cast<off_t>(total));
  map = static_cast<char*>(mmap(nullptr, total, PROT_WRITE, MAP_SHARED, fd, 0));

  int n = static_cast<int>(records.size());
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    std::memcpy(map + static_cast<size_t>(i) * kRecordSize, records[i].data(), kRecordSize);
    if ((i + 1) % kSyncInterval == 0) fdatasync(fd);
  }
  if (n % kSyncInterval != 0) fdatasync(fd);
  auto end = std::chrono::steady_clock::now();
  munmap(map, total);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// --- io_uring benchmarks ---

// io_uring: write + fsync linked per record — one submission, kernel chains them
static auto bench_iouring_every_sync(const std::vector<std::vector<char>>& records,
                                     const std::string& path) -> BenchResult {
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  struct io_uring ring;
  io_uring_queue_init(64, &ring, 0);

  // Warmup
  off_t offset = 0;
  for (int i = 0; i < kWarmupRecords; ++i) {
    auto* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, fd, records[i].data(), kRecordSize, offset);
    sqe->flags |= IOSQE_IO_LINK;
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    io_uring_submit(&ring);
    struct io_uring_cqe* cqe;
    io_uring_wait_cqe(&ring, &cqe);
    io_uring_cqe_seen(&ring, cqe);
    io_uring_wait_cqe(&ring, &cqe);
    io_uring_cqe_seen(&ring, cqe);
    offset += kRecordSize;
  }

  offset = 0;
  auto start = std::chrono::steady_clock::now();
  for (auto& rec : records) {
    auto* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, fd, rec.data(), kRecordSize, offset);
    sqe->flags |= IOSQE_IO_LINK;
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    io_uring_submit(&ring);
    struct io_uring_cqe* cqe;
    io_uring_wait_cqe(&ring, &cqe);
    io_uring_cqe_seen(&ring, cqe);
    io_uring_wait_cqe(&ring, &cqe);
    io_uring_cqe_seen(&ring, cqe);
    offset += kRecordSize;
  }
  auto end = std::chrono::steady_clock::now();
  io_uring_queue_exit(&ring);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

// io_uring: batch N writes then one fsync, all submitted together
static auto bench_iouring_batch_sync(const std::vector<std::vector<char>>& records,
                                     const std::string& path) -> BenchResult {
  auto total = static_cast<off_t>(records.size()) * kRecordSize;
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  fallocate(fd, 0, 0, total);
  ftruncate(fd, total);

  struct io_uring ring;
  io_uring_queue_init(256, &ring, 0);

  // Warmup
  off_t offset = 0;
  for (int i = 0; i < kWarmupRecords; ++i) {
    auto* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, fd, records[i].data(), kRecordSize, offset);
    if ((i + 1) % kSyncInterval == 0) {
      sqe->flags |= IOSQE_IO_LINK;
      sqe = io_uring_get_sqe(&ring);
      io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
      io_uring_submit(&ring);
      struct io_uring_cqe* cqe;
      for (int j = 0; j <= kSyncInterval; ++j) {
        io_uring_wait_cqe(&ring, &cqe);
        io_uring_cqe_seen(&ring, cqe);
      }
    }
    offset += kRecordSize;
  }

  int n = static_cast<int>(records.size());
  offset = 0;
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    auto* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_write(sqe, fd, records[i].data(), kRecordSize, offset);
    offset += kRecordSize;
    if ((i + 1) % kSyncInterval == 0) {
      sqe->flags |= IOSQE_IO_LINK;
      sqe = io_uring_get_sqe(&ring);
      io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
      io_uring_submit(&ring);
      struct io_uring_cqe* cqe;
      for (int j = 0; j <= kSyncInterval; ++j) {
        io_uring_wait_cqe(&ring, &cqe);
        io_uring_cqe_seen(&ring, cqe);
      }
    }
  }
  // Final sync for trailing records
  if (n % kSyncInterval != 0) {
    int tail = n % kSyncInterval;
    auto* sqe = io_uring_get_sqe(&ring);
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    io_uring_submit(&ring);
    struct io_uring_cqe* cqe;
    for (int j = 0; j <= tail; ++j) {
      io_uring_wait_cqe(&ring, &cqe);
      io_uring_cqe_seen(&ring, cqe);
    }
  }
  auto end = std::chrono::steady_clock::now();
  io_uring_queue_exit(&ring);
  close(fd);

  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double ops = static_cast<double>(records.size()) / (ms / 1000.0);
  double mb = (static_cast<double>(records.size()) * kRecordSize) / (1024.0 * 1024.0) / (ms / 1000.0);
  return {ms, ops, mb};
}

static void print_result(const char* name, const BenchResult& r) {
  std::printf("  %-42s %10.1f ms  %10.0f ops/s  %8.2f MiB/s\n",
              name, r.elapsed_ms, r.ops_per_sec, r.mb_per_sec);
}

int main() {
  auto tmp = std::filesystem::current_path() / "./tmp/append_bench_tmp";
  std::filesystem::remove_all(tmp);
  std::filesystem::create_directories(tmp);

  int slug_counter = 0;
  auto run = [&](const char* label, const char* slug, auto&& fn) {
    auto path = (tmp / (std::to_string(slug_counter++) + "_" + slug + ".dat")).string();
    std::filesystem::remove(path);
    auto result = fn(path);
    print_result(label, result);
    std::filesystem::remove(path);
  };

  std::printf("Generating %d records of %d bytes...\n", kNumRecords, kRecordSize);
  auto records = generate_records(kNumRecords, kRecordSize);

  std::printf("\n=== Sync every write ===\n");
  run("writev + fdatasync", "writev_fdatasync",
      [&](const std::string& p) { return bench_writev_every_sync(records, p); });
  run("writev + O_DSYNC", "writev_odsync",
      [&](const std::string& p) { return bench_writev_odsync(records, p); });
  run("writev block-rewrite O_DIRECT|O_DSYNC", "writev_direct_dsync_block",
      [&](const std::string& p) { return bench_writev_direct_dsync_block(records, p); });
  run("pwritev2 + RWF_DSYNC", "pwritev2_rwf_dsync",
      [&](const std::string& p) { return bench_pwritev2_rwf_dsync(records, p); });
  run("mmap + msync(MS_SYNC)", "mmap_msync",
      [&](const std::string& p) { return bench_mmap_every_sync(records, p); });
  run("mmap + fdatasync", "mmap_fdatasync",
      [&](const std::string& p) { return bench_mmap_fdatasync_every(records, p); });
  run("io_uring write+fsync linked", "iouring_every",
      [&](const std::string& p) { return bench_iouring_every_sync(records, p); });

  std::printf("\n=== Sync every %d writes ===\n", kSyncInterval);
  run("writev + fdatasync (1-per-1 IO, batch sync)", "writev_batch_sync",
      [&](const std::string& p) { return bench_writev_batch_sync(records, p); });
  run("writev batched IO + fdatasync", "writev_batched_io",
      [&](const std::string& p) { return bench_writev_batched_io(records, p); });
  run("writev batched + O_DIRECT | O_DSYNC", "writev_direct_dsync_batched",
      [&](const std::string& p) { return bench_writev_direct_dsync_batched(records, p); });
  run("mmap + msync(MS_SYNC)", "mmap_msync_batch",
      [&](const std::string& p) { return bench_mmap_batch_sync(records, p); });
  run("mmap + fdatasync", "mmap_fdatasync_batch",
      [&](const std::string& p) { return bench_mmap_fdatasync_batch(records, p); });
  run("io_uring write+fsync batched", "iouring_batch",
      [&](const std::string& p) { return bench_iouring_batch_sync(records, p); });

  std::filesystem::remove_all(tmp);
  std::printf("\nDone.\n");
}
