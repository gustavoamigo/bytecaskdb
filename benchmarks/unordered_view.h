// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// UnorderedView — linear hashing layer over ByteCaskDB.
//
// Maps arbitrary key distributions (UUIDv4, SHA256, random binary) into
// sequential bucket keys that the radix tree handles efficiently. Built
// entirely on the ByteCaskDB public API; no engine internals required.
//
// Storage layout:
//   Key:   <ns>/b/<4B bucket_id><2B hash16>
//   Value: chain of [{key_len:4B}{original_key}{val_len:4B}{user_value}]+
//          val_len == 0 signals a tombstone. Multiple entries arise only
//          on hash16 collisions (expected rate: ~1/65536 per bucket slot).
//
// get resolves to a single db.get() point-lookup — one disk read, no scan.
// put is a pure append when the slot is absent (the common case); a
// read-modify-write only when the hash16 slot is already occupied.
//
// This is a benchmark prototype — not part of the library yet.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <unordered_map>
#include <span>
#include <string>
#include <vector>

import bytecask;

namespace unordered_view {

// ---------------------------------------------------------------------------
// MurmurHash3 — 32-bit finalizer (Austin Appleby, public domain)
// ---------------------------------------------------------------------------

inline auto murmur3_32(const std::byte *data, std::size_t len,
                       std::uint32_t seed) -> std::uint32_t {
  auto getblock = [](const std::byte *p, std::size_t i) -> std::uint32_t {
    std::uint32_t val;
    std::memcpy(&val, p + i * 4, 4);
    return val;
  };

  auto fmix32 = [](std::uint32_t h) -> std::uint32_t {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
  };

  const std::size_t nblocks = len / 4;
  std::uint32_t h1 = seed;

  constexpr std::uint32_t c1 = 0xcc9e2d51;
  constexpr std::uint32_t c2 = 0x1b873593;

  for (std::size_t i = 0; i < nblocks; ++i) {
    auto k1 = getblock(data, i);
    k1 *= c1;
    k1 = (k1 << 15) | (k1 >> 17);
    k1 *= c2;
    h1 ^= k1;
    h1 = (h1 << 13) | (h1 >> 19);
    h1 = h1 * 5 + 0xe6546b64;
  }

  const auto *tail = data + nblocks * 4;
  std::uint32_t k1 = 0;
  switch (len & 3) {
  case 3:
    k1 ^= static_cast<std::uint32_t>(std::to_integer<uint8_t>(tail[2])) << 16;
    [[fallthrough]];
  case 2:
    k1 ^= static_cast<std::uint32_t>(std::to_integer<uint8_t>(tail[1])) << 8;
    [[fallthrough]];
  case 1:
    k1 ^= static_cast<std::uint32_t>(std::to_integer<uint8_t>(tail[0]));
    k1 *= c1;
    k1 = (k1 << 15) | (k1 >> 17);
    k1 *= c2;
    h1 ^= k1;
  }

  h1 ^= static_cast<std::uint32_t>(len);
  return fmix32(h1);
}

// ---------------------------------------------------------------------------
// BytesHash — hash functor for bytecask::Bytes (std::vector<std::byte>),
// reuses murmur3_32 already in this file.
// ---------------------------------------------------------------------------

struct BytesHash {
  auto operator()(const std::vector<std::byte> &v) const noexcept
      -> std::size_t {
    return murmur3_32(v.data(), v.size(), 0x12345678);
  }
};

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
  std::uint32_t initial_size = 64;     // power of 2
  std::uint32_t bucket_capacity = 128;  // entries per bucket before split
  float load_factor = 0.75f;
  std::uint32_t hash_seed = 0;
};

// ---------------------------------------------------------------------------
// Stats — allocation and I/O counters for diagnosing overhead.
// ---------------------------------------------------------------------------

struct Stats {
  std::uint64_t splits{0};              // bucket splits performed
  std::uint64_t split_empty{0};         // splits that found an empty bucket
  std::uint64_t split_entries_moved{0}; // entries re-routed across all splits
  std::uint64_t split_tombstones_gc{0}; // tombstones GC'd during splits
  std::uint64_t split_db_reads{0};      // db.get calls inside split
  std::uint64_t split_db_writes{0};     // plan put/del ops inside split
  std::uint64_t put_append{0};          // puts into an empty slot (no read)
  std::uint64_t put_chain_update{0};    // puts that read-modify-write a chain
  std::uint64_t chain_decodes{0};       // decode_chain calls (each = heap alloc)
  std::uint64_t chain_encodes{0};       // encode_chain_entry calls
};

// ---------------------------------------------------------------------------
// Chain encoding: zero or more entries packed end-to-end.
//   entry = [key_len:4B LE][original_key:key_len][val_len:4B LE][user_value:val_len]
// val_len == 0 is a tombstone.
// ---------------------------------------------------------------------------

struct ChainEntry {
  std::vector<std::byte> original_key;
  std::vector<std::byte> user_value; // empty = tombstone
};

// Non-owning view into a chain blob. Valid only while the backing buffer lives.
struct ChainEntryView {
  bytecask::BytesView original_key;
  bytecask::BytesView user_value; // empty = tombstone
};

// Serialise one entry into chain wire format.
inline auto encode_chain_entry(bytecask::BytesView key,
                               bytecask::BytesView value) -> bytecask::Bytes {
  auto key_len = static_cast<std::uint32_t>(key.size());
  auto val_len = static_cast<std::uint32_t>(value.size());
  bytecask::Bytes buf(4 + key.size() + 4 + value.size());
  std::memcpy(buf.data(), &key_len, 4);
  std::memcpy(buf.data() + 4, key.data(), key.size());
  std::memcpy(buf.data() + 4 + key.size(), &val_len, 4);
  if (val_len > 0)
    std::memcpy(buf.data() + 4 + key.size() + 4, value.data(), value.size());
  return buf;
}

// Append one chain entry directly into an existing buffer (no intermediate alloc).
inline void encode_chain_entry_into(bytecask::Bytes &buf,
                                    bytecask::BytesView key,
                                    bytecask::BytesView value) {
  auto key_len = static_cast<std::uint32_t>(key.size());
  auto val_len = static_cast<std::uint32_t>(value.size());
  auto off = buf.size();
  buf.resize(off + 4 + key.size() + 4 + value.size());
  std::memcpy(buf.data() + off, &key_len, 4);
  std::memcpy(buf.data() + off + 4, key.data(), key.size());
  std::memcpy(buf.data() + off + 4 + key.size(), &val_len, 4);
  if (val_len > 0)
    std::memcpy(buf.data() + off + 4 + key.size() + 4, value.data(),
                value.size());
}

// Parse a chain blob into its constituent entries.
inline auto decode_chain(bytecask::BytesView chain) -> std::vector<ChainEntry> {
  std::vector<ChainEntry> entries;
  std::size_t pos = 0;
  while (pos + 4 <= chain.size()) {
    std::uint32_t key_len;
    std::memcpy(&key_len, chain.data() + pos, 4);
    pos += 4;
    if (pos + key_len + 4 > chain.size()) break;
    auto key_bytes = chain.subspan(pos, key_len);
    pos += key_len;
    std::uint32_t val_len;
    std::memcpy(&val_len, chain.data() + pos, 4);
    pos += 4;
    if (pos + val_len > chain.size()) break;
    auto val_bytes = chain.subspan(pos, val_len);
    pos += val_len;
    entries.push_back({
        {key_bytes.begin(), key_bytes.end()},
        {val_bytes.begin(), val_bytes.end()},
    });
  }
  return entries;
}

// Zero-copy: returns views into the chain buffer. Caller must keep chain alive.
inline auto decode_chain_views(bytecask::BytesView chain)
    -> std::vector<ChainEntryView> {
  std::vector<ChainEntryView> entries;
  std::size_t pos = 0;
  while (pos + 4 <= chain.size()) {
    std::uint32_t key_len;
    std::memcpy(&key_len, chain.data() + pos, 4);
    pos += 4;
    if (pos + key_len + 4 > chain.size()) break;
    auto key_bytes = chain.subspan(pos, key_len);
    pos += key_len;
    std::uint32_t val_len;
    std::memcpy(&val_len, chain.data() + pos, 4);
    pos += 4;
    if (pos + val_len > chain.size()) break;
    auto val_bytes = chain.subspan(pos, val_len);
    pos += val_len;
    entries.push_back({key_bytes, val_bytes});
  }
  return entries;
}

// Walk a chain looking for a specific original key. Calls visitor(value_view)
// on the last matching entry (last-write-wins). Zero allocation.
// Returns true if a live entry was found, false if absent or tombstoned.
template <typename Visitor>
inline auto find_in_chain(bytecask::BytesView chain, bytecask::BytesView needle,
                          Visitor &&visitor) -> bool {
  bytecask::BytesView last_value{};
  bool found = false;
  std::size_t pos = 0;
  while (pos + 4 <= chain.size()) {
    std::uint32_t key_len;
    std::memcpy(&key_len, chain.data() + pos, 4);
    pos += 4;
    if (pos + key_len + 4 > chain.size()) break;
    auto key_bytes = chain.subspan(pos, key_len);
    pos += key_len;
    std::uint32_t val_len;
    std::memcpy(&val_len, chain.data() + pos, 4);
    pos += 4;
    if (pos + val_len > chain.size()) break;
    auto val_bytes = chain.subspan(pos, val_len);
    pos += val_len;
    if (key_bytes.size() == needle.size() &&
        std::equal(key_bytes.begin(), key_bytes.end(), needle.begin())) {
      last_value = val_bytes;
      found = true;
    }
  }
  if (found) {
    if (last_value.empty()) return false; // tombstone
    visitor(last_value);
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// UnorderedView
// ---------------------------------------------------------------------------

class UnorderedView {
public:
  UnorderedView(bytecask::DB &db, std::string ns, Options opts = {})
      : db_{db}, ns_{std::move(ns)}, opts_{opts},
        initial_size_{opts_.initial_size}, split_pointer_{0}, round_{0},
        version_{0}, entry_count_{0} {
    meta_key_ = ns_ + "/__meta__";
    bucket_prefix_ = ns_ + "/b/";

    // Try to load existing metadata.
    bytecask::Bytes meta_buf;
    if (db_.get({}, to_bv(meta_key_), meta_buf)) {
      load_metadata(meta_buf);
    } else {
      // Write initial metadata.
      auto meta = serialize_metadata();
      db_.put({.sync = false}, to_bv(meta_key_),
              bytecask::BytesView{meta.data(), meta.size()});
    }

    // Recompute entry_count via keys-only scan (in-memory, no disk I/O).
    recompute_entry_count();
  }

  [[nodiscard]] auto stats() const -> const Stats & { return stats_; }

  void put(bytecask::BytesView key, bytecask::BytesView value) {
    auto bucket = route(key);
    auto fp = hash16(key);
    auto sk = make_slot_key(bucket, fp);

    auto new_entry = encode_chain_entry(key, value);
    ++stats_.chain_encodes;
    auto &existing = tl_read_buf();
    if (!db_.get({.verify_checksums = false}, sk.view(), existing)) {
      // Slot absent — pure append (the common case).
      db_.put({.sync = false}, sk.view(), bytecask::BytesView{new_entry});
      ++entry_count_;
      ++stats_.put_append;
    } else {
      // Slot occupied — update matching entry or append a new one.
      ++stats_.put_chain_update;
      auto entries = decode_chain(bytecask::BytesView{existing});
      ++stats_.chain_decodes;
      bool found = false;
      bytecask::Bytes new_chain;
      for (auto &e : entries) {
        auto ek = bytecask::BytesView{e.original_key.data(), e.original_key.size()};
        if (!found && spans_equal(ek, key)) {
          new_chain.insert(new_chain.end(), new_entry.begin(), new_entry.end());
          found = true;
        } else {
          auto reenc = encode_chain_entry(
              bytecask::BytesView{e.original_key.data(), e.original_key.size()},
              bytecask::BytesView{e.user_value.data(), e.user_value.size()});
          ++stats_.chain_encodes;
          new_chain.insert(new_chain.end(), reenc.begin(), reenc.end());
        }
      }
      if (!found) {
        new_chain.insert(new_chain.end(), new_entry.begin(), new_entry.end());
        ++entry_count_;
      }
      db_.put({.sync = false}, sk.view(), bytecask::BytesView{new_chain});
    }

    auto num_buckets = current_num_buckets();
    auto threshold = static_cast<std::size_t>(
        static_cast<float>(num_buckets * opts_.bucket_capacity) *
        opts_.load_factor);
    if (entry_count_ > threshold) {
      split();
    }
  }

  auto get(bytecask::BytesView key, bytecask::Bytes &out) -> bool {
    auto bucket = route(key);
    auto fp = hash16(key);
    auto sk = make_slot_key(bucket, fp);

    auto &chain_bytes = tl_read_buf();
    if (!db_.get({.verify_checksums = false}, sk.view(), chain_bytes))
      return false;

    return find_in_chain(bytecask::BytesView{chain_bytes}, key,
                         [&out](bytecask::BytesView val) {
                           out.assign(val.begin(), val.end());
                         });
  }

  auto contains_key(bytecask::BytesView key) -> bool {
    auto bucket = route(key);
    auto fp = hash16(key);
    auto sk = make_slot_key(bucket, fp);

    auto &chain_bytes = tl_read_buf();
    if (!db_.get({.verify_checksums = false}, sk.view(), chain_bytes))
      return false;

    return find_in_chain(bytecask::BytesView{chain_bytes}, key,
                         [](bytecask::BytesView) {});
  }

  void del(bytecask::BytesView key) {
    auto bucket = route(key);
    auto fp = hash16(key);
    auto sk = make_slot_key(bucket, fp);

    auto tombstone = encode_chain_entry(key, {});
    auto &existing = tl_read_buf();
    if (!db_.get({.verify_checksums = false}, sk.view(), existing)) {
      db_.put({.sync = false}, sk.view(), bytecask::BytesView{tombstone});
    } else {
      auto entries = decode_chain(bytecask::BytesView{existing});
      bool found = false;
      bytecask::Bytes new_chain;
      for (auto &e : entries) {
        auto ek = bytecask::BytesView{e.original_key.data(), e.original_key.size()};
        if (!found && spans_equal(ek, key)) {
          new_chain.insert(new_chain.end(), tombstone.begin(), tombstone.end());
          found = true;
        } else {
          auto reenc = encode_chain_entry(
              bytecask::BytesView{e.original_key.data(), e.original_key.size()},
              bytecask::BytesView{e.user_value.data(), e.user_value.size()});
          new_chain.insert(new_chain.end(), reenc.begin(), reenc.end());
        }
      }
      if (!found)
        new_chain.insert(new_chain.end(), tombstone.begin(), tombstone.end());
      db_.put({.sync = false}, sk.view(), bytecask::BytesView{new_chain});
    }
    --entry_count_;
  }

private:
  static constexpr std::size_t kMetaBytes = 16; // 4 × uint32_t
  // Seed for hash16 — different from routing hash for independence.
  static constexpr std::uint32_t kFpSeed = 0x9E3779B9;

  bytecask::DB &db_;
  std::string ns_;
  Options opts_;

  // In-memory metadata copy.
  std::uint32_t initial_size_;
  std::uint32_t split_pointer_;
  std::uint32_t round_;
  std::uint32_t version_;
  std::size_t entry_count_;

  // Cached key prefixes.
  std::string meta_key_;
  std::string bucket_prefix_;

  // Allocation / I/O counters.
  Stats stats_;

  // 16-bit fingerprint of an original key (different seed from routing hash).
  static auto hash16(bytecask::BytesView key) -> std::uint16_t {
    return static_cast<std::uint16_t>(
        murmur3_32(key.data(), key.size(), kFpSeed));
  }

  // Route a key to its bucket ID.
  auto route(bytecask::BytesView key) const -> std::uint32_t {
    auto h = murmur3_32(key.data(), key.size(), opts_.hash_seed);
    auto n = initial_size_ * (1u << round_);
    auto bucket = h % n;
    if (bucket < split_pointer_) {
      bucket = h % (2 * n);
    }
    return bucket;
  }

  auto current_num_buckets() const -> std::uint32_t {
    return initial_size_ * (1u << round_) + split_pointer_;
  }

  // --- Big-endian encoding (preserves lexicographic order) ---

  static void write_be32(std::byte *dst, std::uint32_t v) {
    dst[0] = static_cast<std::byte>((v >> 24) & 0xFF);
    dst[1] = static_cast<std::byte>((v >> 16) & 0xFF);
    dst[2] = static_cast<std::byte>((v >> 8) & 0xFF);
    dst[3] = static_cast<std::byte>(v & 0xFF);
  }

  static void write_be16(std::byte *dst, std::uint16_t v) {
    dst[0] = static_cast<std::byte>((v >> 8) & 0xFF);
    dst[1] = static_cast<std::byte>(v & 0xFF);
  }

  // Stack-allocated slot key: <ns>/b/<4B bucket BE><2B hash16 BE>.
  // Max namespace length 48 chars → max key 57 bytes. Zero heap allocation.
  static constexpr std::size_t kMaxSlotKeyBytes = 64;

  struct SlotKey {
    std::array<std::byte, kMaxSlotKeyBytes> buf;
    std::size_t len;

    auto view() const -> bytecask::BytesView {
      return bytecask::BytesView{buf.data(), len};
    }
  };

  // Format a full slot key into a stack buffer.
  auto make_slot_key(std::uint32_t bucket_id, std::uint16_t fp) const
      -> SlotKey {
    SlotKey sk{};
    auto prefix_len = bucket_prefix_.size();
    sk.len = prefix_len + 4 + 2;
    std::memcpy(sk.buf.data(),
                reinterpret_cast<const std::byte *>(bucket_prefix_.data()),
                prefix_len);
    write_be32(sk.buf.data() + prefix_len, bucket_id);
    write_be16(sk.buf.data() + prefix_len + 4, fp);
    return sk;
  }

  // Format just the bucket prefix (for prefix scans during split/recount).
  auto make_bucket_prefix_key(std::uint32_t bucket_id) const -> SlotKey {
    SlotKey sk{};
    auto prefix_len = bucket_prefix_.size();
    sk.len = prefix_len + 4;
    std::memcpy(sk.buf.data(),
                reinterpret_cast<const std::byte *>(bucket_prefix_.data()),
                prefix_len);
    write_be32(sk.buf.data() + prefix_len, bucket_id);
    return sk;
  }

  auto serialize_metadata() const -> std::array<std::byte, kMetaBytes> {
    std::array<std::byte, kMetaBytes> buf{};
    auto write_u32 = [&](std::size_t off, std::uint32_t v) {
      std::memcpy(buf.data() + off, &v, 4);
    };
    write_u32(0, initial_size_);
    write_u32(4, split_pointer_);
    write_u32(8, round_);
    write_u32(12, version_);
    return buf;
  }

  void load_metadata(const bytecask::Bytes &buf) {
    if (buf.size() < kMetaBytes)
      return;
    auto read_u32 = [&](std::size_t off) -> std::uint32_t {
      std::uint32_t v;
      std::memcpy(&v, buf.data() + off, 4);
      return v;
    };
    initial_size_ = read_u32(0);
    split_pointer_ = read_u32(4);
    round_ = read_u32(8);
    version_ = read_u32(12);
  }

  void recompute_entry_count() {
    entry_count_ = 0;
    auto prefix_bv = to_bv(bucket_prefix_);
    auto &chain_bytes = tl_read_buf();
    for (auto &key : db_.keys_from({}, prefix_bv)) {
      auto key_span = key_obj_to_bv(key);
      if (!starts_with(key_span, prefix_bv))
        break;
      // Count actual user entries per slot, not just slot keys.
      chain_bytes.clear();
      if (!db_.get({.verify_checksums = false}, key_span, chain_bytes))
        continue;
      auto entries = decode_chain_views(bytecask::BytesView{chain_bytes});
      for (auto &e : entries) {
        if (!e.user_value.empty()) // skip tombstones
          ++entry_count_;
      }
    }
  }

  void split() {
    auto n = initial_size_ * (1u << round_);
    auto old_prefix = make_bucket_prefix_key(split_pointer_);
    auto old_prefix_bv = old_prefix.view();

    // Phase 1: keys-only scan to collect all slot keys (in-memory, free).
    // std::string for SSO — slot keys are ~11 bytes, fit inline.
    std::vector<std::string> old_slot_keys;
    for (auto &key : db_.keys_from({}, old_prefix_bv)) {
      auto key_span = key_obj_to_bv(key);
      if (!starts_with(key_span, old_prefix_bv))
        break;
      old_slot_keys.emplace_back(
          reinterpret_cast<const char *>(key_span.data()), key_span.size());
    }

    if (old_slot_keys.empty()) {
      ++stats_.splits;
      ++stats_.split_empty;
      advance_split_pointer(n);
      return;
    }

    // Phase 2: read each slot chain from disk, re-route entries by new bucket,
    // and build new chain bytes directly — no intermediate survivors map.
    // Tombstones are GC'd during split. Deduplication (last-write-wins) is
    // handled per-slot via a seen set.
    // std::string keys for SSO; std::hash<std::string> for the map.
    std::unordered_map<std::string, bytecask::Bytes> new_chains;
    auto &chain_bytes = tl_read_buf();
    for (auto &slot_key : old_slot_keys) {
      chain_bytes.clear();
      ++stats_.split_db_reads;
      auto slot_bv = std::as_bytes(std::span{slot_key.data(), slot_key.size()});
      if (!db_.get({.verify_checksums = false}, slot_bv, chain_bytes))
        continue;

      // Parse chain entries and deduplicate in one pass (last write wins).
      // Reverse scan so the first occurrence of each key is the latest.
      // Views are valid while chain_bytes lives — processed before next get().
      auto entries = decode_chain_views(bytecask::BytesView{chain_bytes});
      ++stats_.chain_decodes;
      std::vector<bool> keep(entries.size(), false);
      // Small set for dedup within one slot (typically 1-2 entries).
      std::vector<bytecask::BytesView> seen;
      for (auto i = static_cast<int>(entries.size()) - 1; i >= 0; --i) {
        auto ek = entries[static_cast<std::size_t>(i)].original_key;
        bool dup = false;
        for (auto &s : seen) {
          if (spans_equal(s, ek)) { dup = true; break; }
        }
        if (!dup) {
          keep[static_cast<std::size_t>(i)] = true;
          seen.push_back(ek);
        }
      }

      auto new_modulus = 2 * n;
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!keep[i]) continue;
        auto &e = entries[i];
        if (e.user_value.empty()) { ++stats_.split_tombstones_gc; continue; }

        auto h = murmur3_32(e.original_key.data(), e.original_key.size(),
                            opts_.hash_seed);
        auto new_bucket = h % new_modulus;
        auto fp = hash16(e.original_key);
        auto sk = make_slot_key(new_bucket, fp);
        std::string sk_str(reinterpret_cast<const char *>(sk.buf.data()),
                           sk.len);

        ++stats_.chain_encodes;
        ++stats_.split_entries_moved;
        encode_chain_entry_into(new_chains[sk_str], e.original_key,
                                e.user_value);
      }
    }

    // Phase 3: build atomic write plan.
    ++stats_.splits;
    stats_.split_db_writes += old_slot_keys.size(); // deletes
    stats_.split_db_writes += new_chains.size();    // puts
    stats_.split_db_writes += 1;                    // meta put
    auto snap = db_.snapshot();
    bytecask::WritePlan plan{std::move(snap)};
    plan.ensure_unchanged(to_bv(meta_key_));

    for (auto &slot_key : old_slot_keys) {
      plan.del(std::as_bytes(std::span{slot_key.data(), slot_key.size()}));
    }

    for (auto &[new_slot_key, chain] : new_chains) {
      plan.put(
          std::as_bytes(std::span{new_slot_key.data(), new_slot_key.size()}),
          bytecask::BytesView{chain.data(), chain.size()});
    }

    // Update metadata.
    auto new_split = split_pointer_ + 1;
    auto new_round = round_;
    if (new_split >= n) {
      new_split = 0;
      new_round = round_ + 1;
    }
    auto new_version = version_ + 1;

    auto saved_split = split_pointer_;
    auto saved_round = round_;
    auto saved_version = version_;

    split_pointer_ = new_split;
    round_ = new_round;
    version_ = new_version;
    auto new_meta = serialize_metadata();
    plan.put(to_bv(meta_key_),
             bytecask::BytesView{new_meta.data(), new_meta.size()});

    if (!db_.apply_batch({.sync = false}, std::move(plan))) {
      // CAS conflict — rollback in-memory state and reload.
      split_pointer_ = saved_split;
      round_ = saved_round;
      version_ = saved_version;
      bytecask::Bytes meta_buf;
      if (db_.get({}, to_bv(meta_key_), meta_buf)) {
        load_metadata(meta_buf);
      }
    }
  }

  void advance_split_pointer(std::uint32_t n) {
    auto new_split = split_pointer_ + 1;
    auto new_round = round_;
    if (new_split >= n) {
      new_split = 0;
      new_round = round_ + 1;
    }
    split_pointer_ = new_split;
    round_ = new_round;
    version_ = version_ + 1;
    auto new_meta = serialize_metadata();
    db_.put({.sync = false}, to_bv(meta_key_),
            bytecask::BytesView{new_meta.data(), new_meta.size()});
  }

  // --- Helpers ---

  // Reusable per-thread read buffer — avoids alloc/free on every get/put/split.
  // Safe: reads are lock-free across threads, writes are serialized by the engine.
  static auto tl_read_buf() -> bytecask::Bytes & {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wexit-time-destructors"
#pragma clang diagnostic ignored "-Wunique-object-duplication"
    thread_local bytecask::Bytes buf;
#pragma clang diagnostic pop
    buf.clear();
    return buf;
  }

  static auto to_bv(const std::string &s) -> bytecask::BytesView {
    return std::as_bytes(std::span{s.data(), s.size()});
  }

  static auto key_obj_to_bv(const bytecask::Key &k) -> bytecask::BytesView {
    return bytecask::BytesView{
        reinterpret_cast<const std::byte *>(&*k.begin()), k.size()};
  }

  static auto starts_with(bytecask::BytesView haystack,
                           bytecask::BytesView prefix) -> bool {
    if (haystack.size() < prefix.size())
      return false;
    return std::equal(prefix.begin(), prefix.end(), haystack.begin());
  }

  static auto spans_equal(bytecask::BytesView a,
                           bytecask::BytesView b) -> bool {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin());
  }
};

} // namespace unordered_view
