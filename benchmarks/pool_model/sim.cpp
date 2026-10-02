// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// Offline model of the buffer pool under a write-heavy workload
// (exp/pool-write-model). Replays a BYTECASK_POOL_TRACE file — record reads,
// appends, superseded records, vacuum's new and deleted files — through the
// pool's rules and through variants of them, at any pool size.
//
// Unlike the read-only replay of issue #148, it knows how much of each 4 KiB
// frame is still live: an append makes its bytes live, a kill (a record
// superseded) makes them dead, vacuum's compacted file starts fully live and
// the file it replaced disappears. Policies can use that.
//
//   sim sort  <trace.bin> <sorted.bin>        order records by timestamp
//   sim stats <sorted.bin> <phases.txt>
//   sim run   <sorted.bin> <phases.txt> <pool_mib> <policy> [<policy>...]
//
// Counted from the "run" phase on; everything before warms the model.
// Output: one CSV row per policy.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <queue>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint64_t kFrame = 4096;
constexpr std::uint64_t kBlockFrames = 128 * 1024 / kFrame;

struct Rec {
  std::uint64_t ts;
  std::uint64_t off;
  std::uint32_t fid;
  std::uint32_t len;
  std::uint8_t kind;
  std::uint8_t ctx;
  std::uint16_t pad;
  std::uint32_t tid;
};
static_assert(sizeof(Rec) == 32);
enum Kind : std::uint8_t { kRead = 0, kAppend = 1, kKill = 2, kNewFile = 3, kUnlink = 4, kLive = 5 };
enum Ctx : std::uint8_t { kOther = 0, kGet = 1, kWrite = 2 };

auto key_of(std::uint32_t fid, std::uint64_t frame) -> std::uint64_t {
  return (std::uint64_t{fid} << 32) | frame;
}

struct Trace {
  const Rec *r = nullptr;
  std::size_t n = 0;
};

auto map_trace(const char *path) -> Trace {
  const int fd = ::open(path, O_RDONLY);
  if (fd < 0) { std::perror(path); std::exit(1); }
  struct stat st {};
  ::fstat(fd, &st);
  void *p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  if (p == MAP_FAILED) { std::perror("mmap"); std::exit(1); }
  ::madvise(p, static_cast<std::size_t>(st.st_size), MADV_SEQUENTIAL);
  return {static_cast<const Rec *>(p), static_cast<std::size_t>(st.st_size) / sizeof(Rec)};
}

auto phase_ts(const char *path, const std::string &name) -> std::uint64_t {
  std::ifstream in(path);
  std::string k;
  std::uint64_t v = 0;
  while (in >> k >> v) if (k == name) return v;
  std::fprintf(stderr, "no phase %s\n", name.c_str());
  std::exit(1);
}

// ---------------------------------------------------------------------------
// Liveness: bytes of live records per frame, per file.
// ---------------------------------------------------------------------------
struct Files {
  struct File {
    std::vector<std::uint32_t> live;  // per frame
    std::uint64_t size = 0;
    std::uint64_t live_total = 0;
    std::uint64_t created_seq = 0;    // order of creation
  };
  std::unordered_map<std::uint32_t, File> f;
  std::uint64_t next_seq = 0;
  std::uint64_t live_total = 0, file_total = 0;

  auto get(std::uint32_t fid) -> File & {
    auto [it, fresh] = f.try_emplace(fid);
    if (fresh) it->second.created_seq = next_seq++;
    return it->second;
  }
  template <typename Fn>
  static void each_frame(std::uint64_t off, std::uint64_t len, Fn fn) {
    std::uint64_t end = off + len;
    while (off < end) {
      const auto fr = off / kFrame;
      const auto stop = std::min(end, (fr + 1) * kFrame);
      fn(fr, stop - off);
      off = stop;
    }
  }
  void extend(std::uint32_t fid, std::uint64_t end) {
    auto &x = get(fid);
    if (end > x.size) { file_total += end - x.size; x.size = end; }
    const auto frames = (x.size + kFrame - 1) / kFrame;
    if (x.live.size() < frames) x.live.resize(frames, 0);
  }
  void add_live(std::uint32_t fid, std::uint64_t off, std::uint64_t len) {
    extend(fid, off + len);
    auto &x = get(fid);
    each_frame(off, len, [&](std::uint64_t fr, std::uint64_t n) {
      x.live[fr] += static_cast<std::uint32_t>(n);
    });
    x.live_total += len; live_total += len;
  }
  // Returns frames whose live bytes reached zero.
  template <typename Fn>
  void kill(std::uint32_t fid, std::uint64_t off, std::uint64_t len, Fn on_dead) {
    auto it = f.find(fid);
    if (it == f.end()) return;
    auto &x = it->second;
    each_frame(off, len, [&](std::uint64_t fr, std::uint64_t n) {
      if (fr >= x.live.size()) return;
      const auto d = std::min<std::uint64_t>(n, x.live[fr]);
      x.live[fr] -= static_cast<std::uint32_t>(d);
      x.live_total -= d; live_total -= d;
      if (x.live[fr] == 0 && d > 0) on_dead(fr);
    });
  }
  void unlink(std::uint32_t fid) {
    auto it = f.find(fid);
    if (it == f.end()) return;
    live_total -= it->second.live_total;
    file_total -= it->second.size;
    f.erase(it);
  }
  auto frame_live(std::uint32_t fid, std::uint64_t fr) const -> std::uint32_t {
    auto it = f.find(fid);
    if (it == f.end() || fr >= it->second.live.size()) return 0;
    return it->second.live[fr];
  }
  auto file_density(std::uint32_t fid) const -> double {
    auto it = f.find(fid);
    if (it == f.end() || it->second.size == 0) return 0;
    return static_cast<double>(it->second.live_total) / static_cast<double>(it->second.size);
  }
};

// ---------------------------------------------------------------------------
// Policies.
// ---------------------------------------------------------------------------
struct Policy {
  std::string name;
  enum Fill { kPoolFill, kAlwaysBlock, kDenseFileBlock, kYoungFileBlock, kLiveFilteredBlock } fill = kPoolFill;
  double dense = 0.5;         // file density threshold for kDenseFileBlock
  std::uint64_t young = 4;    // newest N files count as young
  bool writer_visited = true; // writer's frames admitted visited (the pool)
  bool dead_first = false;    // evict frames with no live bytes before anything else
  double sparse_no_second_chance = -1; // frames below this live fraction ignore their visited bit
  bool vacuum_warm = false;   // vacuum's moved records enter the pool, as the writer's appends do
};

auto parse_policy(const std::string &s) -> Policy {
  Policy p;
  p.name = s;
  // Tokens joined by '+'.
  std::size_t i = 0;
  while (i <= s.size()) {
    auto j = s.find('+', i);
    if (j == std::string::npos) j = s.size();
    const auto t = s.substr(i, j - i);
    if (t == "pool") {}
    else if (t == "ra") p.fill = Policy::kAlwaysBlock;
    else if (t.rfind("ra_dense", 0) == 0) { p.fill = Policy::kDenseFileBlock; if (t.size() > 8) p.dense = std::stod(t.substr(9)); }
    else if (t.rfind("ra_young", 0) == 0) { p.fill = Policy::kYoungFileBlock; if (t.size() > 8) p.young = std::stoull(t.substr(9)); }
    else if (t == "ra_live") p.fill = Policy::kLiveFilteredBlock;
    else if (t == "wnv") p.writer_visited = false;
    else if (t == "vacwarm") p.vacuum_warm = true;
    else if (t == "deadfirst") p.dead_first = true;
    else if (t.rfind("sparse", 0) == 0) p.sparse_no_second_chance = std::stod(t.substr(7));
    else if (t == "opt") {}
    else { std::fprintf(stderr, "unknown policy token %s\n", t.c_str()); std::exit(1); }
    i = j + 1;
  }
  return p;
}

struct Counters {
  std::uint64_t reads[3] = {}, misses[3] = {};
  std::uint64_t frame_misses = 0, cold = 0, capacity = 0;
  std::uint64_t io_ops = 0, io_bytes = 0;
  std::uint64_t admitted_prefetch = 0, prefetch_used = 0;
  double miss_frame_density = 0;   // sum of live fraction of missed frames
  double miss_file_density = 0;
  std::uint64_t vacuum_new_bytes = 0, vacuum_unlinked_bytes = 0;
};

// SIEVE over frames, with pinning of the active file and liveness-aware variants.
class Sim {
public:
  Sim(std::size_t frames, Policy p) : cap_{frames}, p_{std::move(p)} {
    node_.reserve(frames);
    map_.reserve(frames * 2);
  }

  void run(const Trace &t, std::uint64_t count_from, Counters &c, Files &files) {
    for (std::size_t i = 0; i < t.n; ++i) {
      const auto &r = t.r[i];
      counting_ = r.ts >= count_from;
      switch (r.kind) {
        case kAppend: {
          if (r.fid != active_) { active_ = r.fid; }
          files.extend(r.fid, r.off + r.len);
          Files::each_frame(r.off, r.len, [&](std::uint64_t fr, std::uint64_t) {
            const auto k = key_of(r.fid, fr);
            auto it = map_.find(k);
            if (it != map_.end()) { node_[it->second].visited |= p_.writer_visited; return; }
            admit(k, r.fid, p_.writer_visited, files, /*prefetch=*/false);
          });
          break;
        }
        case kLive:
          files.add_live(r.fid, r.off, r.len);
          if (p_.vacuum_warm && vacuum_files_.count(r.fid)) {
            Files::each_frame(r.off, r.len, [&](std::uint64_t fr, std::uint64_t) {
              const auto k = key_of(r.fid, fr);
              if (!map_.count(k)) admit(k, r.fid, false, files, /*prefetch=*/false);
            });
          }
          break;
        case kKill:
          files.kill(r.fid, r.off, r.len, [&](std::uint64_t fr) {
            if (p_.dead_first) {
              auto it = map_.find(key_of(r.fid, fr));
              if (it != map_.end()) dead_.push_back(key_of(r.fid, fr));
            }
          });
          break;
        case kNewFile:
          files.extend(r.fid, r.len);
          vacuum_files_.insert(r.fid);
          if (counting_) c.vacuum_new_bytes += r.len;
          break;
        case kUnlink: {
          auto it = files.f.find(r.fid);
          if (it != files.f.end()) {
            if (counting_) c.vacuum_unlinked_bytes += it->second.size;
            const auto nfr = it->second.live.size();
            for (std::uint64_t fr = 0; fr < nfr; ++fr) drop(key_of(r.fid, fr));
          }
          files.unlink(r.fid);
          break;
        }
        case kRead:
          read(r, c, files);
          break;
        default: break;
      }
    }
  }

private:
  struct Node {
    std::uint64_t key;
    std::uint32_t fid;
    bool visited;
    bool prefetched;
    std::int64_t prev, next;  // toward newer / older
  };
  std::size_t cap_;
  Policy p_;
  std::vector<Node> node_;
  std::vector<std::int64_t> free_;
  std::unordered_map<std::uint64_t, std::int64_t> map_;
  std::int64_t newest_ = -1, oldest_ = -1, hand_ = -1;
  std::uint32_t active_ = UINT32_MAX;
  std::vector<std::uint64_t> dead_;
  std::unordered_set<std::uint64_t> seen_;
  std::unordered_set<std::uint32_t> vacuum_files_;
  bool counting_ = false;

  auto size() const -> std::size_t { return map_.size(); }

  void link_newest(std::int64_t i) {
    node_[i].prev = -1;
    node_[i].next = newest_;
    if (newest_ >= 0) node_[newest_].prev = i;
    newest_ = i;
    if (oldest_ < 0) oldest_ = i;
  }
  void unlink_node(std::int64_t i) {
    auto &n = node_[i];
    if (hand_ == i) hand_ = n.prev;  // hand moves toward newer
    if (n.prev >= 0) node_[n.prev].next = n.next; else newest_ = n.next;
    if (n.next >= 0) node_[n.next].prev = n.prev; else oldest_ = n.prev;
  }
  void drop(std::uint64_t k) {
    auto it = map_.find(k);
    if (it == map_.end()) return;
    unlink_node(it->second);
    free_.push_back(it->second);
    map_.erase(it);
  }
  auto evict_one(Files &files) -> bool {
    while (p_.dead_first && !dead_.empty()) {
      const auto k = dead_.back();
      dead_.pop_back();
      auto it = map_.find(k);
      if (it == map_.end()) continue;
      if (node_[it->second].fid == active_) continue;
      if (files.frame_live(node_[it->second].fid, k & 0xFFFFFFFF) != 0) continue;
      drop(k);
      return true;
    }
    std::size_t guard = 0;
    for (;;) {
      if (hand_ < 0) hand_ = oldest_;
      if (hand_ < 0) return false;
      auto &n = node_[hand_];
      const auto here = hand_;
      hand_ = n.prev;
      if (n.fid == active_) {
        if (++guard > 4 * cap_) {
          std::fprintf(stderr, "pool smaller than the active file: below the engine's 2x floor\n");
          std::exit(3);
        }
        continue;
      }
      bool keep = n.visited;
      if (keep && p_.sparse_no_second_chance >= 0) {
        const double frac = files.frame_live(n.fid, n.key & 0xFFFFFFFF) / static_cast<double>(kFrame);
        if (frac < p_.sparse_no_second_chance) keep = false;
      }
      if (keep) { n.visited = false; if (++guard > 4 * cap_) return false; continue; }
      drop(n.key);
      (void)here;
      return true;
    }
  }
  void admit(std::uint64_t k, std::uint32_t fid, bool visited, Files &files, bool prefetch) {
    if (size() >= cap_ && !evict_one(files)) return;
    std::int64_t i;
    if (!free_.empty()) { i = free_.back(); free_.pop_back(); }
    else { i = static_cast<std::int64_t>(node_.size()); node_.push_back({}); }
    node_[i] = Node{k, fid, visited, prefetch, -1, -1};
    link_newest(i);
    map_.emplace(k, i);
  }

  auto block_fill(std::uint32_t fid, Files &files) const -> bool {
    switch (p_.fill) {
      case Policy::kPoolFill: return size() < cap_;
      case Policy::kAlwaysBlock: return true;
      case Policy::kLiveFilteredBlock: return true;
      case Policy::kDenseFileBlock: return files.file_density(fid) >= p_.dense;
      case Policy::kYoungFileBlock: {
        auto it = files.f.find(fid);
        return it != files.f.end() && it->second.created_seq + p_.young >= files.next_seq;
      }
    }
    return false;
  }

  void read(const Rec &r, Counters &c, Files &files) {
    bool missed = false;
    std::uint64_t first_missing = UINT64_MAX, last_missing = 0;
    Files::each_frame(r.off, r.len, [&](std::uint64_t fr, std::uint64_t) {
      const auto k = key_of(r.fid, fr);
      auto it = map_.find(k);
      if (it != map_.end()) {
        auto &n = node_[it->second];
        if (n.prefetched && counting_) { ++c.prefetch_used; }
        n.prefetched = false;
        n.visited = true;
        return;
      }
      missed = true;
      first_missing = std::min(first_missing, fr);
      last_missing = std::max(last_missing, fr);
      if (counting_) {
        ++c.frame_misses;
        if (seen_.count(k)) ++c.capacity; else ++c.cold;
        c.miss_frame_density += files.frame_live(r.fid, fr) / static_cast<double>(kFrame);
        c.miss_file_density += files.file_density(r.fid);
      }
    });
    Files::each_frame(r.off, r.len, [&](std::uint64_t fr, std::uint64_t) { seen_.insert(key_of(r.fid, fr)); });
    if (counting_) { ++c.reads[r.ctx % 3]; if (missed) ++c.misses[r.ctx % 3]; }
    if (!missed) return;
    // Fill: the missing frames, widened to their 128 KiB blocks when the policy says so.
    const auto file_frames = [&] {
      auto it = files.f.find(r.fid);
      return it == files.f.end() ? last_missing + 1 : (it->second.size + kFrame - 1) / kFrame;
    }();
    std::uint64_t a = first_missing, b = last_missing;
    const bool block = block_fill(r.fid, files);
    if (block) {
      a = first_missing / kBlockFrames * kBlockFrames;
      b = std::min<std::uint64_t>((last_missing / kBlockFrames + 1) * kBlockFrames, file_frames) - 1;
    }
    if (counting_) { ++c.io_ops; c.io_bytes += (b - a + 1) * kFrame; }
    for (auto fr = a; fr <= b; ++fr) {
      const auto k = key_of(r.fid, fr);
      if (map_.count(k)) continue;
      const bool demanded = fr >= first_missing && fr <= last_missing;
      if (!demanded && p_.fill == Policy::kLiveFilteredBlock &&
          files.frame_live(r.fid, fr) < kFrame / 2) continue;
      admit(k, r.fid, demanded, files, !demanded);
      if (!demanded && counting_) ++c.admitted_prefetch;
    }
  }
};

// Belady (OPT with bypass) over the frame-level stream: reads and appends.
void run_opt(const Trace &t, std::uint64_t count_from, std::size_t cap, Counters &c) {
  // Frame-level event list.
  std::vector<std::uint64_t> keys;
  std::vector<std::uint8_t> is_read;
  std::vector<std::uint8_t> counted;
  std::vector<std::uint32_t> rec_of;
  keys.reserve(t.n);
  for (std::size_t i = 0; i < t.n; ++i) {
    const auto &r = t.r[i];
    if (r.kind != kRead && r.kind != kAppend) continue;
    Files::each_frame(r.off, r.len, [&](std::uint64_t fr, std::uint64_t) {
      keys.push_back(key_of(r.fid, fr));
      is_read.push_back(r.kind == kRead);
      counted.push_back(r.ts >= count_from);
      rec_of.push_back(static_cast<std::uint32_t>(i));
    });
  }
  const auto n = keys.size();
  std::vector<std::uint64_t> next(n);
  {
    std::unordered_map<std::uint64_t, std::uint64_t> last;
    last.reserve(n / 4);
    for (std::size_t i = n; i-- > 0;) {
      auto it = last.find(keys[i]);
      next[i] = it == last.end() ? UINT64_MAX : it->second;
      last[keys[i]] = i;
    }
  }
  std::unordered_map<std::uint64_t, std::uint64_t> cached;  // key -> next use
  cached.reserve(cap * 2);
  std::priority_queue<std::pair<std::uint64_t, std::uint64_t>> heap;  // (next, key)
  std::uint32_t last_rec = UINT32_MAX;
  bool rec_missed = false;
  const Rec *cur = nullptr;
  auto close_rec = [&] {
    if (cur && cur->kind == kRead && cur->ts >= count_from) {
      ++c.reads[cur->ctx % 3];
      if (rec_missed) ++c.misses[cur->ctx % 3];
    }
  };
  for (std::size_t i = 0; i < n; ++i) {
    if (rec_of[i] != last_rec) { close_rec(); last_rec = rec_of[i]; cur = &t.r[rec_of[i]]; rec_missed = false; }
    const auto k = keys[i];
    auto it = cached.find(k);
    if (it != cached.end()) {
      it->second = next[i];
      heap.emplace(next[i], k);
      continue;
    }
    if (is_read[i]) { rec_missed = true; if (counted[i]) ++c.frame_misses; }
    if (next[i] == UINT64_MAX) continue;  // never used again: bypass
    if (cached.size() >= cap) {
      // Find the cached key with the farthest next use (lazy heap).
      for (;;) {
        auto [nx, kk] = heap.top();
        auto jt = cached.find(kk);
        if (jt == cached.end() || jt->second != nx) { heap.pop(); continue; }
        if (nx <= next[i]) goto bypass;  // everything cached is needed sooner
        heap.pop();
        cached.erase(jt);
        break;
      }
    }
    cached.emplace(k, next[i]);
    heap.emplace(next[i], k);
    continue;
  bypass:;
  }
  close_rec();
}

void print_header() {
  std::printf("policy,pool_mib,reads,miss_ratio,get_miss_ratio,write_miss_ratio,iter_miss_ratio,"
              "frame_misses,cold,capacity,miss_frame_live,miss_file_live,io_ops,io_mib,"
              "prefetch_admitted,prefetch_used,vacuum_new_mib,vacuum_unlinked_mib,"
              "live_mib_end,file_mib_end\n");
}

void print_row(const std::string &name, std::uint64_t pool_mib, const Counters &c,
               const Files *files) {
  const auto reads = c.reads[0] + c.reads[1] + c.reads[2];
  const auto misses = c.misses[0] + c.misses[1] + c.misses[2];
  auto ratio = [](std::uint64_t a, std::uint64_t b) { return b ? static_cast<double>(a) / static_cast<double>(b) : 0.0; };
  std::printf("%s,%llu,%llu,%.5f,%.5f,%.5f,%.5f,%llu,%llu,%llu,%.3f,%.3f,%llu,%.1f,%llu,%llu,%.1f,%.1f,%.1f,%.1f\n",
              name.c_str(), static_cast<unsigned long long>(pool_mib),
              static_cast<unsigned long long>(reads), ratio(misses, reads),
              ratio(c.misses[kGet], c.reads[kGet]), ratio(c.misses[kWrite], c.reads[kWrite]),
              ratio(c.misses[kOther], c.reads[kOther]),
              static_cast<unsigned long long>(c.frame_misses), static_cast<unsigned long long>(c.cold),
              static_cast<unsigned long long>(c.capacity),
              c.frame_misses ? c.miss_frame_density / static_cast<double>(c.frame_misses) : 0.0,
              c.frame_misses ? c.miss_file_density / static_cast<double>(c.frame_misses) : 0.0,
              static_cast<unsigned long long>(c.io_ops), static_cast<double>(c.io_bytes) / 1048576.0,
              static_cast<unsigned long long>(c.admitted_prefetch), static_cast<unsigned long long>(c.prefetch_used),
              static_cast<double>(c.vacuum_new_bytes) / 1048576.0, static_cast<double>(c.vacuum_unlinked_bytes) / 1048576.0,
              files ? static_cast<double>(files->live_total) / 1048576.0 : 0.0,
              files ? static_cast<double>(files->file_total) / 1048576.0 : 0.0);
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: see header\n"); return 2; }
  const std::string mode = argv[1];
  if (mode == "sort" && argc == 4) {
    const auto t = map_trace(argv[2]);
    std::vector<Rec> v(t.r, t.r + t.n);
    std::stable_sort(v.begin(), v.end(), [](const Rec &a, const Rec &b) { return a.ts < b.ts; });
    FILE *o = std::fopen(argv[3], "wb");
    std::fwrite(v.data(), sizeof(Rec), v.size(), o);
    std::fclose(o);
    std::printf("%zu records\n", v.size());
    return 0;
  }
  if (mode == "stats" && argc == 4) {
    const auto t = map_trace(argv[2]);
    const auto run_ts = phase_ts(argv[3], "run");
    const auto end_ts = phase_ts(argv[3], "end");
    std::uint64_t kinds[6] = {}, ctx_reads[3] = {}, read_bytes = 0, app_bytes = 0, kill_bytes = 0;
    Files files;
    std::uint64_t live_at_run = 0, file_at_run = 0, live_frames_at_run = 0;
    bool marked = false;
    auto live_frames = [&] {
      std::uint64_t n = 0;
      for (const auto &[fid, x] : files.f)
        for (auto v : x.live) n += v > 0;
      return n;
    };
    // Range scans: a thread's run of iterator reads with no gap over 200 us.
    struct Seq { std::uint64_t last_ts = 0, reads = 0, bytes = 0; std::vector<std::uint64_t> frames; };
    std::unordered_map<std::uint32_t, Seq> seqs;
    std::uint64_t scans = 0, scan_reads = 0, scan_frames = 0, scan_ideal = 0;
    auto close_seq = [&](Seq &q) {
      if (q.reads >= 50) {
        std::sort(q.frames.begin(), q.frames.end());
        const auto distinct = static_cast<std::uint64_t>(
            std::unique(q.frames.begin(), q.frames.end()) - q.frames.begin());
        ++scans; scan_reads += q.reads; scan_frames += distinct;
        scan_ideal += (q.bytes / 2 + kFrame - 1) / kFrame;  // key read + value read per row
      }
      q.reads = 0; q.bytes = 0; q.frames.clear();
    };
    for (std::size_t i = 0; i < t.n; ++i) {
      const auto &r = t.r[i];
      if (!marked && r.ts >= run_ts) { live_at_run = files.live_total; file_at_run = files.file_total; live_frames_at_run = live_frames(); marked = true; }
      if (r.kind == kAppend) files.extend(r.fid, r.off + r.len);
      else if (r.kind == kLive) files.add_live(r.fid, r.off, r.len);
      else if (r.kind == kKill) files.kill(r.fid, r.off, r.len, [](std::uint64_t) {});
      else if (r.kind == kNewFile) files.extend(r.fid, r.len);
      else if (r.kind == kUnlink) files.unlink(r.fid);
      if (r.ts < run_ts) continue;
      ++kinds[r.kind % 6];
      if (r.kind == kRead) { ++ctx_reads[r.ctx % 3]; read_bytes += r.len; }
      if (r.kind == kRead) {
        auto &q = seqs[r.tid];
        if (r.ctx != kOther || r.ts - q.last_ts > 200'000) close_seq(q);
        if (r.ctx == kOther) {
          ++q.reads; q.bytes += r.len;
          Files::each_frame(r.off, r.len, [&](std::uint64_t fr, std::uint64_t) { q.frames.push_back(key_of(r.fid, fr)); });
        }
        q.last_ts = r.ts;
      }
      if (r.kind == kAppend) app_bytes += r.len;
      if (r.kind == kKill) kill_bytes += r.len;
    }
    const double secs = static_cast<double>(end_ts - run_ts) / 1e9;
    std::printf("run window %.1f s\n", secs);
    std::printf("reads %llu (get %llu, write-path %llu, other/iterator %llu), avg %.0f B\n",
                static_cast<unsigned long long>(kinds[0]), static_cast<unsigned long long>(ctx_reads[1]),
                static_cast<unsigned long long>(ctx_reads[2]), static_cast<unsigned long long>(ctx_reads[0]),
                kinds[0] ? static_cast<double>(read_bytes) / static_cast<double>(kinds[0]) : 0.0);
    std::printf("appended %.1f MiB, killed %.1f MiB, new files %llu, unlinked %llu\n",
                static_cast<double>(app_bytes) / 1048576.0, static_cast<double>(kill_bytes) / 1048576.0,
                static_cast<unsigned long long>(kinds[3]), static_cast<unsigned long long>(kinds[4]));
    std::printf("at run start: live %.1f MiB in %.1f MiB of files; at end: live %.1f MiB in %.1f MiB (%zu files)\n",
                static_cast<double>(live_at_run) / 1048576.0, static_cast<double>(file_at_run) / 1048576.0,
                static_cast<double>(files.live_total) / 1048576.0, static_cast<double>(files.file_total) / 1048576.0,
                files.f.size());
    std::printf("frames holding a live record: %.1f MiB at run start, %.1f MiB at end\n",
                static_cast<double>(live_frames_at_run) * kFrame / 1048576.0,
                static_cast<double>(live_frames()) * kFrame / 1048576.0);
    for (auto &[tid, q] : seqs) close_seq(q);
    std::printf("scans (>=50 iterator reads): %llu, avg %.1f reads, %.1f distinct frames (%.1f if the rows were contiguous)\n",
                static_cast<unsigned long long>(scans),
                scans ? static_cast<double>(scan_reads) / static_cast<double>(scans) : 0.0,
                scans ? static_cast<double>(scan_frames) / static_cast<double>(scans) : 0.0,
                scans ? static_cast<double>(scan_ideal) / static_cast<double>(scans) : 0.0);
    return 0;
  }
  if (mode == "header") { print_header(); return 0; }
  if (mode == "run" && argc >= 6) {
    const auto t = map_trace(argv[2]);
    const auto run_ts = phase_ts(argv[3], "run");
    const auto pool_mib = std::strtoull(argv[4], nullptr, 10);
    const auto frames = pool_mib * 1048576 / kFrame;
    if (std::getenv("SIM_NO_HEADER") == nullptr) print_header();
    for (int a = 5; a < argc; ++a) {
      const std::string name = argv[a];
      Counters c;
      if (name == "opt") {
        run_opt(t, run_ts, frames, c);
        print_row(name, pool_mib, c, nullptr);
        continue;
      }
      Files files;
      Sim sim(frames, parse_policy(name));
      sim.run(t, run_ts, c, files);
      print_row(name, pool_mib, c, &files);
    }
    return 0;
  }
  std::fprintf(stderr, "bad arguments\n");
  return 2;
}
