// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — offline replay of a buffer pool trace (issue #148).
//
// Replays a BYTECASK_POOL_TRACE file through CLOCK variants and fill
// policies that libCacheSim cannot model: the pool's own CLOCK, whose hand
// walks the open-addressed index in hash order, next to textbook CLOCK over
// an insertion-ordered ring; demand frames admitted referenced (as the pool
// does) or not; and 128 KiB block fills never, while the pool has free
// frames (today), or always, with the prefetched neighbours admitted
// referenced or unreferenced. The active file is never evicted, as in the
// pool.
//
// It mirrors BufferPool::read_at and append_resident step for step, minus the
// concurrency: a read serves resident frames, fills each run of missing ones
// (widened to its blocks when the fill policy says so), and admits every
// whole frame of the run that is not already resident. An append admits a
// missing frame only when the written bytes start at its first byte.
//
// Output is CSV on stdout, one row per (policy, admission, fill, size).

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint64_t kFrameBytes = 4096;
constexpr std::uint64_t kBlockFrames = 128 * 1024 / kFrameBytes;
constexpr std::uint8_t kActive = 1;
constexpr std::uint8_t kAppend = 2;

struct Record {
  std::uint64_t ts_ns;
  std::uint32_t file_id;
  std::uint32_t first_frame;
  std::uint32_t n_frames;
  std::uint16_t first_offset;
  std::uint8_t kind;
  std::uint8_t pad;
};
static_assert(sizeof(Record) == 24);

auto make_key(std::uint32_t file_id, std::uint64_t frame) -> std::uint64_t {
  return (static_cast<std::uint64_t>(file_id) << 32) | (frame & 0xFFFFFFFFULL);
}

// Frames a BufferPool of this total footprint holds: the same arithmetic as
// its constructor, so a simulated size and a measured one name the same pool.
auto pool_frames(std::size_t capacity_bytes) -> std::size_t {
  constexpr std::size_t kSlot = 16;
  constexpr std::size_t kPerFrame = kFrameBytes + 4;
  const auto wanted = capacity_bytes / (kPerFrame + kSlot);
  const auto table = std::bit_ceil(std::max<std::size_t>(wanted * 10 / 7, 2));
  const auto table_bytes = table * kSlot;
  return std::min(capacity_bytes > table_bytes
                      ? (capacity_bytes - table_bytes) / kPerFrame
                      : 0,
                  table * 7 / 10);
}

// ---------------------------------------------------------------------------
// The pool's CLOCK: open addressing, linear probing, backward-shift erase,
// and a hand that walks table slots — so the sweep order is hash order.
// ---------------------------------------------------------------------------
class HashClock {
public:
  explicit HashClock(std::size_t frames)
      : frames_{frames},
        mask_{std::bit_ceil(std::max<std::size_t>(frames * 10 / 7, 2)) - 1},
        keys_(mask_ + 1, kEmpty), refs_(mask_ + 1, 0) {}

  auto lookup(std::uint64_t key) -> bool {
    const auto i = find(key);
    if (i == kNone) return false;
    refs_[i] = 1;
    return true;
  }
  [[nodiscard]] auto contains(std::uint64_t key) const -> bool {
    return find(key) != kNone;
  }
  [[nodiscard]] auto full() const -> bool { return used_ >= frames_; }

  // Returns false if nothing could be evicted (every frame is the active
  // file's), as admit() does.
  auto admit(std::uint64_t key, bool ref, std::uint32_t active) -> bool {
    if (used_ < frames_) {
      ++used_;
    } else {
      const auto v = victim(active);
      if (v == kNone) return false;
      erase(v);
      ++evictions;
    }
    auto h = home(key);
    while (keys_[h] != kEmpty) h = (h + 1) & mask_;
    keys_[h] = key;
    refs_[h] = ref ? 1 : 0;
    return true;
  }

  std::uint64_t evictions{0};

private:
  static constexpr std::uint64_t kEmpty = ~std::uint64_t{0};
  static constexpr std::size_t kNone = ~std::size_t{0};

  static auto hash_key(std::uint64_t k) -> std::size_t {
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ULL;
    k ^= k >> 27;
    k *= 0x94d049bb133111ebULL;
    k ^= k >> 31;
    return static_cast<std::size_t>(k);
  }
  [[nodiscard]] auto home(std::uint64_t k) const -> std::size_t {
    return hash_key(k) & mask_;
  }
  [[nodiscard]] auto find(std::uint64_t key) const -> std::size_t {
    for (auto h = home(key);; h = (h + 1) & mask_) {
      if (keys_[h] == key) return h;
      if (keys_[h] == kEmpty) return kNone;
    }
  }
  auto victim(std::uint32_t active) -> std::size_t {
    for (std::size_t steps = 0; steps <= 2 * mask_ + 2; ++steps) {
      const auto i = hand_;
      hand_ = (hand_ + 1) & mask_;
      const auto k = keys_[i];
      if (k == kEmpty || (k >> 32) == active) continue;
      if (refs_[i] != 0) {
        refs_[i] = 0;
        continue;
      }
      return i;
    }
    return kNone;
  }
  void erase(std::size_t i) {
    for (auto j = (i + 1) & mask_;; j = (j + 1) & mask_) {
      const auto k = keys_[j];
      if (k == kEmpty) break;
      const auto h = home(k);
      const bool crosses = h <= j ? (h <= i && i < j) : (h <= i || i < j);
      if (!crosses) continue;
      keys_[i] = k;
      refs_[i] = refs_[j];
      i = j;
    }
    keys_[i] = kEmpty;
  }

  std::size_t frames_;
  std::size_t mask_;
  std::vector<std::uint64_t> keys_;
  std::vector<std::uint8_t> refs_;
  std::size_t hand_{0};
  std::size_t used_{0};
};

// ---------------------------------------------------------------------------
// Textbook CLOCK: a ring of frames in insertion order, a new frame taking the
// victim's place behind the hand.
// ---------------------------------------------------------------------------
class RingClock {
public:
  explicit RingClock(std::size_t frames) : ring_(frames), refs_(frames, 0) {
    where_.reserve(frames * 2);
  }

  auto lookup(std::uint64_t key) -> bool {
    const auto it = where_.find(key);
    if (it == where_.end()) return false;
    refs_[it->second] = 1;
    return true;
  }
  [[nodiscard]] auto contains(std::uint64_t key) const -> bool {
    return where_.contains(key);
  }
  [[nodiscard]] auto full() const -> bool { return used_ >= ring_.size(); }

  auto admit(std::uint64_t key, bool ref, std::uint32_t active) -> bool {
    std::size_t slot = 0;
    if (used_ < ring_.size()) {
      slot = used_++;
    } else {
      bool found = false;
      for (std::size_t steps = 0; steps <= 2 * ring_.size(); ++steps) {
        const auto i = hand_;
        hand_ = (hand_ + 1) % ring_.size();
        if ((ring_[i] >> 32) == active) continue;
        if (refs_[i] != 0) {
          refs_[i] = 0;
          continue;
        }
        slot = i;
        found = true;
        break;
      }
      if (!found) return false;
      where_.erase(ring_[slot]);
      ++evictions;
    }
    ring_[slot] = key;
    refs_[slot] = ref ? 1 : 0;
    where_.emplace(key, slot);
    return true;
  }

  std::uint64_t evictions{0};

private:
  std::vector<std::uint64_t> ring_;
  std::vector<std::uint8_t> refs_;
  std::unordered_map<std::uint64_t, std::size_t> where_;
  std::size_t hand_{0};
  std::size_t used_{0};
};

// ---------------------------------------------------------------------------
// SIEVE (Zhang et al., NSDI '24): a FIFO list, newest at the head, and a hand
// that walks from the tail toward the head. A hit sets the visited bit and
// moves nothing — the same hit path as CLOCK. Eviction clears visited frames
// as the hand passes and evicts the first unvisited one; survivors keep their
// place, so new frames sit between the hand and the head and meet it after
// one pass rather than behind every survivor. Links are per slot, touched
// only on admission and eviction. The active file is passed over, as in the
// pool.
// ---------------------------------------------------------------------------
class Sieve {
public:
  explicit Sieve(std::size_t frames)
      : cap_{frames}, keys_(frames), refs_(frames, 0), newer_(frames, kNil),
        older_(frames, kNil) {
    where_.reserve(frames * 2);
  }

  auto lookup(std::uint64_t key) -> bool {
    const auto it = where_.find(key);
    if (it == where_.end()) return false;
    refs_[it->second] = 1;
    return true;
  }
  [[nodiscard]] auto contains(std::uint64_t key) const -> bool {
    return where_.contains(key);
  }
  [[nodiscard]] auto full() const -> bool { return used_ >= cap_; }

  auto admit(std::uint64_t key, bool ref, std::uint32_t active) -> bool {
    std::uint32_t slot = 0;
    if (used_ < cap_) {
      slot = static_cast<std::uint32_t>(used_++);
    } else {
      auto o = hand_ != kNil ? hand_ : tail_;
      bool found = false;
      for (std::size_t steps = 0; steps <= 2 * cap_; ++steps) {
        if ((keys_[o] >> 32) != active) {
          if (refs_[o] == 0) {
            found = true;
            break;
          }
          refs_[o] = 0;
        }
        o = newer_[o] != kNil ? newer_[o] : tail_;
      }
      if (!found) return false;
      hand_ = newer_[o];
      unlink(o);
      where_.erase(keys_[o]);
      slot = o;
      ++evictions;
    }
    keys_[slot] = key;
    refs_[slot] = ref ? 1 : 0;
    push_head(slot);
    where_.emplace(key, slot);
    return true;
  }

  std::uint64_t evictions{0};

private:
  static constexpr std::uint32_t kNil = ~std::uint32_t{0};

  void push_head(std::uint32_t s) {
    older_[s] = head_;
    newer_[s] = kNil;
    if (head_ != kNil) newer_[head_] = s;
    head_ = s;
    if (tail_ == kNil) tail_ = s;
  }
  void unlink(std::uint32_t s) {
    if (newer_[s] != kNil) older_[newer_[s]] = older_[s]; else head_ = older_[s];
    if (older_[s] != kNil) newer_[older_[s]] = newer_[s]; else tail_ = newer_[s];
  }

  std::size_t cap_;
  std::vector<std::uint64_t> keys_;
  std::vector<std::uint8_t> refs_;
  std::vector<std::uint32_t> newer_, older_;
  std::unordered_map<std::uint64_t, std::uint32_t> where_;
  std::uint32_t head_{kNil}, tail_{kNil}, hand_{kNil};
  std::size_t used_{0};
};

enum class Fill { Frames, WhileFree, BlocksRef, BlocksNoRef, BlocksProbation };

// Quick demotion for prefetched frames: a FIFO of kProbationShare of the
// frames, outside the CLOCK. A frame read while in it moves to the CLOCK; one
// that is not falls out after the FIFO turns over, never having cost the
// CLOCK a slot.
constexpr double kProbationShare = 0.1;
class Probation {
public:
  explicit Probation(std::size_t cap) : cap_{std::max<std::size_t>(cap, 1)} {}
  [[nodiscard]] auto contains(std::uint64_t k) const -> bool { return live_.contains(k); }
  // True if k was here; it leaves either way.
  auto take(std::uint64_t k) -> bool { return live_.erase(k) != 0; }
  void push(std::uint64_t k) {
    live_[k] = ++seq_;
    order_.emplace_back(k, seq_);
    while (live_.size() > cap_) {
      const auto [ok, os] = order_.front();
      order_.pop_front();
      if (const auto it = live_.find(ok); it != live_.end() && it->second == os) {
        live_.erase(it);
      }
    }
    // Stale entries (taken keys) are dropped as they reach the front.
    while (!order_.empty()) {
      const auto it = live_.find(order_.front().first);
      if (it != live_.end() && it->second == order_.front().second) break;
      order_.pop_front();
    }
  }

private:
  std::size_t cap_;
  std::uint64_t seq_{0};
  std::unordered_map<std::uint64_t, std::uint64_t> live_;
  std::deque<std::pair<std::uint64_t, std::uint64_t>> order_;
};

auto fill_name(Fill f) -> const char * {
  switch (f) {
  case Fill::Frames: return "frames";
  case Fill::WhileFree: return "blocks_while_free";
  case Fill::BlocksRef: return "blocks_ref";
  case Fill::BlocksNoRef: return "blocks_noref";
  case Fill::BlocksProbation: return "blocks_probation";
  }
  return "?";
}

struct Result {
  std::uint64_t demand_frames{0}, frame_hits{0};
  std::uint64_t reads{0}, read_hits{0};
  std::uint64_t device_reads{0}, device_frames{0}, fills{0}, evictions{0};
};

struct Options {
  std::string trace;
  std::vector<std::size_t> frames;
  std::vector<std::string> policies{"hash", "ring"};
  std::vector<Fill> fills{Fill::Frames, Fill::WhileFree, Fill::BlocksRef,
                          Fill::BlocksNoRef, Fill::BlocksProbation};
  std::vector<int> admit_refs{1};
  bool reads_only = false;  // drop appends and active-file reads (#148 as written)
  double warmup = 0.1;      // fraction of reads before counting starts
  std::uint64_t count_from_ns = 0;  // or: count reads from this timestamp on
};

// Highest frame seen per file across the whole trace: a stand-in for the
// file size, so a block fill does not admit frames past the end of a file.
auto last_frames(const std::vector<Record> &recs)
    -> std::unordered_map<std::uint32_t, std::uint32_t> {
  std::unordered_map<std::uint32_t, std::uint32_t> last;
  for (const auto &r : recs) {
    auto &l = last[r.file_id];
    l = std::max(l, r.first_frame + r.n_frames - 1);
  }
  return last;
}

template <typename Cache>
auto replay(const std::vector<Record> &recs,
            const std::unordered_map<std::uint32_t, std::uint32_t> &last,
            std::size_t frames, Fill fill, bool admit_ref,
            std::size_t count_from) -> Result {
  const bool probation = fill == Fill::BlocksProbation;
  const auto probation_frames =
      probation ? static_cast<std::size_t>(kProbationShare * static_cast<double>(frames)) : 0;
  Cache cache{frames - probation_frames};
  Probation prob{probation_frames};
  const auto resident = [&](std::uint64_t key) {
    return cache.contains(key) || (probation && prob.contains(key));
  };
  Result res;
  auto active = ~std::uint32_t{0};
  std::size_t read_no = 0;
  for (const auto &r : recs) {
    if ((r.kind & kAppend) != 0) {
      active = r.file_id;
      for (std::uint32_t i = 0; i < r.n_frames; ++i) {
        const auto key = make_key(r.file_id, r.first_frame + i);
        if (resident(key)) continue;
        if (i == 0 && r.first_offset != 0) continue;
        (void)cache.admit(key, true, active);
      }
      continue;
    }
    const bool counted = read_no++ >= count_from;
    const auto file_last = last.at(r.file_id);
    bool any_miss = false;
    const auto fill_run = [&](std::uint64_t a, std::uint64_t b) {
      const bool widen = fill == Fill::BlocksRef || fill == Fill::BlocksNoRef ||
                         probation ||
                         (fill == Fill::WhileFree && !cache.full());
      const auto da = a, db = b;
      if (widen) {
        a = a / kBlockFrames * kBlockFrames;
        b = std::min<std::uint64_t>((b / kBlockFrames + 1) * kBlockFrames - 1,
                                    std::max<std::uint64_t>(file_last, db));
      }
      if (counted) {
        ++res.device_reads;
        res.device_frames += b - a + 1;
      }
      for (auto f = a; f <= b; ++f) {
        const auto key = make_key(r.file_id, f);
        if (resident(key)) continue;
        const bool demanded = f >= da && f <= db;
        if (probation && !demanded) {
          prob.push(key);
          if (counted) ++res.fills;
          continue;
        }
        const bool ref = demanded ? admit_ref : fill == Fill::BlocksRef ||
                                                    (fill == Fill::WhileFree && admit_ref);
        if (cache.admit(key, ref, active) && counted) ++res.fills;
      }
    };
    bool in_run = false;
    std::uint64_t run_start = 0;
    const std::uint64_t first = r.first_frame;
    const auto last_f = first + r.n_frames - 1;
    for (auto f = first; f <= last_f; ++f) {
      const auto key = make_key(r.file_id, f);
      bool hit = cache.lookup(key);
      if (!hit && probation && prob.take(key)) {
        hit = true;  // promoted: it earned a CLOCK slot
        (void)cache.admit(key, true, active);
      }
      if (counted) {
        ++res.demand_frames;
        if (hit) ++res.frame_hits;
      }
      if (hit) {
        if (in_run) {
          fill_run(run_start, f - 1);
          in_run = false;
        }
      } else if (!in_run) {
        run_start = f;
        in_run = true;
        any_miss = true;
      }
    }
    if (in_run) fill_run(run_start, last_f);
    if (counted) {
      ++res.reads;
      if (!any_miss) ++res.read_hits;
    }
  }
  res.evictions = cache.evictions;
  return res;
}

auto split(std::string_view s) -> std::vector<std::string> {
  std::vector<std::string> out;
  while (!s.empty()) {
    const auto c = s.find(',');
    out.emplace_back(s.substr(0, c));
    if (c == std::string_view::npos) break;
    s.remove_prefix(c + 1);
  }
  return out;
}

auto parse_fill(const std::string &s) -> Fill {
  if (s == "frames") return Fill::Frames;
  if (s == "blocks_while_free") return Fill::WhileFree;
  if (s == "blocks_ref") return Fill::BlocksRef;
  if (s == "blocks_noref") return Fill::BlocksNoRef;
  if (s == "blocks_probation") return Fill::BlocksProbation;
  std::fprintf(stderr, "unknown fill '%s'\n", s.c_str());
  std::exit(2);
}

} // namespace

auto main(int argc, char **argv) -> int {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--trace") o.trace = next();
    else if (a == "--frames") {
      for (const auto &v : split(next())) o.frames.push_back(std::stoull(v));
    } else if (a == "--capacity-bytes") {
      for (const auto &v : split(next())) o.frames.push_back(pool_frames(std::stoull(v)));
    } else if (a == "--policies") o.policies = split(next());
    else if (a == "--fills") {
      o.fills.clear();
      for (const auto &v : split(next())) o.fills.push_back(parse_fill(v));
    } else if (a == "--admit-ref") {
      o.admit_refs.clear();
      for (const auto &v : split(next())) o.admit_refs.push_back(std::stoi(v));
    } else if (a == "--reads-only") o.reads_only = true;
    else if (a == "--warmup") o.warmup = std::stod(next());
    else if (a == "--count-from-ns") o.count_from_ns = std::stoull(next());
    else {
      std::fprintf(stderr,
                   "pool_trace_sim --trace FILE (--frames a,b | --capacity-bytes a,b)\n"
                   "  [--policies hash,ring,sieve] [--admit-ref 1,0]\n"
                   "  [--warmup 0.1 | --count-from-ns T]  (reads before are not counted)\n"
                   "  [--fills frames,blocks_while_free,blocks_ref,blocks_noref,blocks_probation]\n"
                   "  [--reads-only]   (drop appends and active-file reads)\n");
      return 2;
    }
  }
  if (o.trace.empty() || o.frames.empty()) {
    std::fprintf(stderr, "--trace and a size are required (--help)\n");
    return 2;
  }

  std::vector<Record> recs;
  {
    std::ifstream in{o.trace, std::ios::binary | std::ios::ate};
    if (!in) {
      std::fprintf(stderr, "cannot open %s\n", o.trace.c_str());
      return 1;
    }
    const auto bytes = static_cast<std::size_t>(in.tellg());
    recs.resize(bytes / sizeof(Record));
    in.seekg(0);
    in.read(reinterpret_cast<char *>(recs.data()),
            static_cast<std::streamsize>(recs.size() * sizeof(Record)));
  }
  // Threads write their buffers out in chunks; the timestamp is the order.
  std::stable_sort(recs.begin(), recs.end(),
                   [](const Record &a, const Record &b) { return a.ts_ns < b.ts_ns; });
  if (o.reads_only) {
    std::erase_if(recs, [](const Record &r) { return (r.kind & (kActive | kAppend)) != 0; });
  }
  const auto n_reads = static_cast<std::size_t>(std::count_if(
      recs.begin(), recs.end(), [](const Record &r) { return (r.kind & kAppend) == 0; }));
  const auto count_from =
      o.count_from_ns != 0
          ? static_cast<std::size_t>(std::count_if(
                recs.begin(), recs.end(),
                [&](const Record &r) {
                  return (r.kind & kAppend) == 0 && r.ts_ns < o.count_from_ns;
                }))
          : static_cast<std::size_t>(o.warmup * static_cast<double>(n_reads));
  const auto last = last_frames(recs);
  std::fprintf(stderr, "%zu records, %zu reads, counting from read %zu\n",
               recs.size(), n_reads, count_from);

  std::printf("policy,admit_ref,fill,frames,reads,read_hit_ratio,demand_frames,"
              "frame_hit_ratio,device_reads,device_frames,fills,evictions\n");
  for (const auto frames : o.frames) {
    for (const auto &policy : o.policies) {
      for (const auto admit_ref : o.admit_refs) {
        for (const auto fill : o.fills) {
          const auto r =
              policy == "hash"    ? replay<HashClock>(recs, last, frames, fill, admit_ref != 0, count_from)
              : policy == "sieve" ? replay<Sieve>(recs, last, frames, fill, admit_ref != 0, count_from)
                                  : replay<RingClock>(recs, last, frames, fill, admit_ref != 0, count_from);
          const auto ratio = [](std::uint64_t a, std::uint64_t b) {
            return b == 0 ? 0.0 : static_cast<double>(a) / static_cast<double>(b);
          };
          std::printf("%s,%d,%s,%zu,%llu,%.4f,%llu,%.4f,%llu,%llu,%llu,%llu\n",
                      policy.c_str(), admit_ref, fill_name(fill), frames,
                      static_cast<unsigned long long>(r.reads), ratio(r.read_hits, r.reads),
                      static_cast<unsigned long long>(r.demand_frames),
                      ratio(r.frame_hits, r.demand_frames),
                      static_cast<unsigned long long>(r.device_reads),
                      static_cast<unsigned long long>(r.device_frames),
                      static_cast<unsigned long long>(r.fills),
                      static_cast<unsigned long long>(r.evictions));
          std::fflush(stdout);
        }
      }
    }
  }
  return 0;
}
