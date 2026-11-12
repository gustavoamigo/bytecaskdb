// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — data file ids: leased, and reused once nothing reads the file
// (#774)
//
// A key directory entry names a record by (file id, offset) in 20 bits of
// file id, so a process that kept minting ids would run out after about a
// million rotations and vacuum compactions however little disk it used. An
// id is therefore leased: every DataFile the engine opens for a file holds
// the file's FileId, and the id goes back to its pool when the last one is
// destroyed. Every state, snapshot, iterator, WritePlan and manifest that can
// name the id reaches the file through a DataFile, so none is left when the
// id is reused: a location (file id, offset) still names one record for as
// long as anything can hold it (docs/bytecask_design.md, *File id reuse*).
//
// A FileId also carries a serial that is never reused in the process. Caches
// that outlive the files they describe — the buffer pool's frames and the
// change index's bookmarks — key by the serial, so a reused id never finds
// what the file it named before left behind.

module;
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

export module bytecask.file_id;

namespace bytecask {

// The buffer pool keys a frame by (serial << 22) | frame index (22 bits:
// 16 GiB of 4 KiB frames, past any record a file can hold), so 42 bits remain
// for the serial. At a thousand files a second that is 139 years of a
// process; a real one creates a few a second.
export inline constexpr std::uint64_t kMaxFileSerial =
    (std::uint64_t{1} << 42) - 1;

export class FileIdPool;

export class FileId {
public:
  FileId(const FileId &) = delete;
  auto operator=(const FileId &) -> FileId & = delete;
  ~FileId();

  [[nodiscard]] auto id() const noexcept -> std::uint32_t { return id_; }
  [[nodiscard]] auto serial() const noexcept -> std::uint64_t {
    return serial_;
  }

  // An id that belongs to no pool and goes back nowhere: for a file opened
  // outside an engine (tests, tools). Its serial is fresh all the same.
  [[nodiscard]] static auto unpooled(std::uint32_t id)
      -> std::shared_ptr<const FileId>;

private:
  friend class FileIdPool;
  FileId(std::uint32_t id, std::shared_ptr<FileIdPool> pool);

  std::uint32_t id_;
  std::uint64_t serial_;
  std::shared_ptr<FileIdPool> pool_;  // null for an unpooled id
};

// *id, or std::logic_error naming who wanted a file id and had none.
export [[nodiscard]] inline auto checked_id(
    const std::shared_ptr<const FileId> &id, const char *who) -> const FileId & {
  if (id == nullptr)
    throw std::logic_error{std::string{who} + ": file has no engine id"};
  return *id;
}

// The ids 0..max_id of one DB. Thread-safe: an id comes back on whichever
// thread drops the last DataFile of its file. The DB takes ids under its
// write path only, so a count of free ids taken there stays true until the
// write path takes one: ids only come back in between.
export class FileIdPool : public std::enable_shared_from_this<FileIdPool> {
public:
  explicit FileIdPool(std::uint32_t max_id) : max_id_{max_id} {
    publish_left();
  }

  // The most recently returned id, else the lowest never used (so open
  // numbers files in name order). Reusing first keeps ids, and the file
  // table's pages, dense. Null when every id is leased.
  [[nodiscard]] auto take() -> std::shared_ptr<const FileId>;

  // How many more ids take() can give now. Grows as ids come back. One load,
  // no lock: the write path reads it for every commit group.
  [[nodiscard]] auto left() const noexcept -> std::uint32_t {
    return left_.load(std::memory_order_relaxed);
  }

  // Starts the never-used ids at id, so a test reaches the ceiling without
  // creating a million files. Only forward: an id below next_ may be leased.
  void test_set_next(std::uint32_t id) {
    std::lock_guard<std::mutex> lk{mu_};
    if (id < next_)
      throw std::logic_error{"FileIdPool: ids only move forward"};
    next_ = id;
    publish_left();
  }

private:
  friend class FileId;
  // Cannot fail: take() reserved room for every id it has handed out.
  void give_back(std::uint32_t id) noexcept;
  // Stores what left() reads. Called under mu_ by every change.
  void publish_left() noexcept {
    const auto unused = next_ <= max_id_ ? max_id_ - next_ + 1 : 0;
    left_.store(static_cast<std::uint32_t>(unused + free_.size()),
                std::memory_order_relaxed);
  }

  mutable std::mutex mu_;
  std::uint32_t max_id_;
  std::uint64_t next_{0};    // lowest never-used id; 64-bit so it can pass max_id_
  std::uint64_t minted_{0};  // never-used ids handed out: free_ holds at most this many
  std::vector<std::uint32_t> free_;
  std::atomic<std::uint32_t> left_{0};
};

namespace {
// Process-wide, so serials are unique across DBs too.
std::atomic<std::uint64_t> next_file_serial{0};

auto mint_serial() -> std::uint64_t {
  const auto s = next_file_serial.fetch_add(1, std::memory_order_relaxed);
  if (s > kMaxFileSerial)
    throw std::runtime_error{"bytecask: file serials exhausted"};
  return s;
}
}  // namespace

FileId::FileId(std::uint32_t id, std::shared_ptr<FileIdPool> pool)
    : id_{id}, serial_{mint_serial()}, pool_{std::move(pool)} {}

FileId::~FileId() {
  if (pool_) pool_->give_back(id_);
}

auto FileId::unpooled(std::uint32_t id) -> std::shared_ptr<const FileId> {
  return std::shared_ptr<const FileId>{new FileId{id, nullptr}};
}

auto FileIdPool::take() -> std::shared_ptr<const FileId> {
  std::lock_guard<std::mutex> lk{mu_};
  std::uint32_t id = 0;
  if (!free_.empty()) {
    id = free_.back();
  } else if (next_ <= max_id_) {
    id = static_cast<std::uint32_t>(next_);
    // Room for every id handed out, so give_back never allocates.
    if (free_.capacity() < minted_ + 1)
      free_.reserve(std::max<std::size_t>(64, 2 * free_.capacity()));
  } else {
    return nullptr;
  }
  // Built without its pool, so a failed allocation gives nothing back (and
  // does not take mu_ again); attached once nothing below can throw.
  auto lease = std::shared_ptr<FileId>{new FileId{id, nullptr}};
  lease->pool_ = shared_from_this();
  if (!free_.empty()) {
    free_.pop_back();
  } else {
    ++next_;
    ++minted_;
  }
  publish_left();
  return lease;
}

void FileIdPool::give_back(std::uint32_t id) noexcept {
  std::lock_guard<std::mutex> lk{mu_};
  free_.push_back(id);
  publish_left();
}

}  // namespace bytecask
