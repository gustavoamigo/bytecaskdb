// Standalone stress of the I/O pattern behind the engine's active file, with
// no engine code: one writer appends records into a file zero-filled to its
// capacity and seals it by truncating it to its logical end, while readers
// pread the newest records of it and of the files sealed just before it.
// Every record's bytes are a function of (file, offset), so a read that
// returns anything else is caught and reported. Exit 1 on the first bad read.
//
//   clang++ -std=c++20 -O2 -pthread tests/pread_truncate_stress.cpp
//   ./a.out <dir> <seconds> [no-zero-fill] [zero-fill-4k] [no-sync]
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kCapacity = 1 << 20;
constexpr std::size_t kPage = 4096;
constexpr int kOpenFiles = 4;

auto byte_at(std::uint32_t file, std::uint64_t offset) -> unsigned char {
  auto x = (std::uint64_t{file} << 40) ^ offset;
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 29;
  return static_cast<unsigned char>(x | 1);  // never 0, unlike the zero fill
}

// Closed by the last holder, so a reader never preads a reused descriptor.
struct File {
  File() = default;
  File(const File &) = delete;
  auto operator=(const File &) -> File & = delete;
  ~File() {
    if (fd != -1) ::close(fd);
  }
  std::uint32_t id{0};
  int fd{-1};
  std::atomic<std::uint64_t> end{0};  // published: bytes below it are written
  std::atomic<std::uint64_t> last{0}; // start of the newest record
};

std::mutex files_mu;
std::vector<std::shared_ptr<File>> files;  // newest last; copied under files_mu
std::atomic<bool> stop{false};
std::atomic<std::uint64_t> reads{0};

void fail(const File &f, std::uint64_t off, std::size_t len,
          const std::vector<unsigned char> &buf) {
  std::size_t bad = 0;
  while (bad < len && buf[bad] == byte_at(f.id, off + bad)) ++bad;
  std::vector<unsigned char> now(len);
  const auto n = ::pread(f.fd, now.data(), len, static_cast<off_t>(off));
  std::size_t now_bad = 0;
  while (now_bad < static_cast<std::size_t>(n) &&
         now[now_bad] == byte_at(f.id, off + now_bad))
    ++now_bad;
  // Where in the file do the bytes that were read belong?
  long long found = -1;
  for (std::uint64_t at = 0; at + 16 <= f.end.load(); ++at) {
    bool eq = true;
    for (std::size_t i = 0; i < 16 && eq; ++i)
      eq = byte_at(f.id, at + i) == buf[bad + i];
    if (eq) { found = static_cast<long long>(at); break; }
  }
  std::fprintf(stderr,
               "BAD READ file %u offset %llu len %zu end %llu: first bad byte "
               "at +%zu (got %02x want %02x); bytes there belong at %lld "
               "(delta %lld); reread now %s at +%zu\n",
               f.id, static_cast<unsigned long long>(off), len,
               static_cast<unsigned long long>(f.end.load()), bad, buf[bad],
               byte_at(f.id, off + bad), found,
               found < 0 ? 0LL
                         : found - static_cast<long long>(off + bad),
               now_bad >= static_cast<std::size_t>(n) ? "correct" : "wrong",
               now_bad);
  std::exit(1);
}

void reader(unsigned seed) {
  std::mt19937_64 rng{seed};
  std::vector<unsigned char> buf;
  std::vector<std::shared_ptr<File>> snap;
  while (!stop.load(std::memory_order_relaxed)) {
    {
      std::lock_guard<std::mutex> lk{files_mu};
      snap = files;
    }
    if (snap.empty()) continue;
    // Mostly the active file's newest record, as the engine's readers do
    // right after a put; sometimes a file sealed just before it.
    const auto &f = snap[snap.size() - 1 -
                         (rng() % 4 == 0 ? rng() % snap.size() : 0)];
    const auto last = f->last.load(std::memory_order_acquire);
    const auto end = f->end.load(std::memory_order_acquire);
    if (end == 0) continue;
    const auto off = rng() % 2 ? last : rng() % end;
    // Like fetch_record: from the offset to the end of its page, bounded by
    // the published end.
    const auto len = static_cast<std::size_t>(
        std::min<std::uint64_t>(kPage - off % kPage, end - off));
    buf.resize(len);
    const auto n = ::pread(f->fd, buf.data(), len, static_cast<off_t>(off));
    if (n != static_cast<ssize_t>(len)) {
      std::fprintf(stderr, "short pread %zd of %zu: %s\n", n, len,
                   std::strerror(errno));
      std::exit(1);
    }
    for (std::size_t i = 0; i < len; ++i)
      if (buf[i] != byte_at(f->id, off + i)) fail(*f, off, len, buf);
    reads.fetch_add(1, std::memory_order_relaxed);
  }
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <dir> <seconds> [no-zero-fill] [zero-fill-4k] [no-sync]\n",
                 argv[0]);
    return 2;
  }
  const std::string dir = argv[1];
  const auto seconds = std::atoi(argv[2]);
  bool zero_fill = true;
  bool zero_fill_4k = false;
  bool sync = true;
  for (int i = 3; i < argc; ++i) {
    if (std::string{argv[i]} == "no-zero-fill") zero_fill = false;
    if (std::string{argv[i]} == "zero-fill-4k") zero_fill_4k = true;
    if (std::string{argv[i]} == "no-sync") sync = false;
  }
  std::vector<std::thread> readers;
  for (unsigned i = 0; i < 3; ++i) readers.emplace_back(reader, 17 + i);

  std::mt19937_64 rng{42};
  const std::vector<unsigned char> zeros(kCapacity, 0);
  std::vector<unsigned char> rec;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{seconds};
  std::uint32_t next_id = 1;
  while (std::chrono::steady_clock::now() < deadline) {
    auto f = std::make_shared<File>();
    f->id = next_id++;
    const auto path = dir + "/f" + std::to_string(f->id) + ".data";
    f->fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (f->fd < 0) { std::perror("open"); return 2; }
    ::posix_fadvise(f->fd, 0, 0, POSIX_FADV_RANDOM);
    // One write of the whole capacity, as ensure_zeroed does, or one page
    // per write.
    const auto piece = zero_fill_4k ? kPage : kCapacity;
    for (std::size_t at = 0; zero_fill && at < kCapacity; at += piece) {
      if (::pwrite(f->fd, zeros.data(), piece, static_cast<off_t>(at)) !=
          static_cast<ssize_t>(piece)) {
        std::perror("zero fill");
        return 2;
      }
    }
    {
      std::lock_guard<std::mutex> lk{files_mu};
      files.push_back(f);
      if (files.size() > kOpenFiles) files.erase(files.begin());
    }
    // Sealed at 10–100 KiB, as resume() and small max_file_bytes seal.
    const auto seal_at = 10 * 1024 + rng() % (90 * 1024);
    std::uint64_t end = 0;
    while (end < seal_at) {
      const auto size = 20 + rng() % 400;
      rec.resize(size);
      for (std::size_t i = 0; i < size; ++i) rec[i] = byte_at(f->id, end + i);
      if (::pwrite(f->fd, rec.data(), size, static_cast<off_t>(end)) !=
          static_cast<ssize_t>(size)) { std::perror("append"); return 2; }
      if (sync && rng() % 8 == 0) ::fdatasync(f->fd);
      const auto start = end;
      end += size;
      // end before last: a reader that sees a record's start sees its end.
      f->end.store(end, std::memory_order_release);
      f->last.store(start, std::memory_order_release);
    }
    // Seal: drop the zero-filled tail while readers read the record just
    // written, as shrink_to_fit and resume() do.
    if (sync) ::fdatasync(f->fd);
    if (::ftruncate(f->fd, static_cast<off_t>(end)) != 0) {
      std::perror("ftruncate");
      return 2;
    }
    if (sync) ::fdatasync(f->fd);
    // A file past the readers' set is unlinked; the last reader closes it.
    ::unlink((dir + "/f" + std::to_string(f->id - kOpenFiles) + ".data").c_str());
  }
  stop = true;
  for (auto &t : readers) t.join();
  std::printf("ok: %u files, %llu reads\n", next_id - 1,
              static_cast<unsigned long long>(reads.load()));
  return 0;
}
