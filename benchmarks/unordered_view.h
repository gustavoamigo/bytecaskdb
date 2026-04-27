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
//   Key:   <ns>/b/<4B bucket_id><4B timestamp><2B hash16>
//   Value: [key_len: 4-byte LE u32][original_key][value]
//
// The 16-bit fingerprint (hash16) in the key enables keys-only filtering:
// get scans bucket keys in reverse (newest first) via rkeys_from — pure
// in-memory radix tree walk — and only fetches values from disk when the
// fingerprint matches (1/65536 false positive rate).
//
// This is a benchmark prototype — not part of the library yet.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
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
// Options
// ---------------------------------------------------------------------------

struct Options {
  std::uint32_t initial_size = 64;     // power of 2
  std::uint32_t bucket_capacity = 8;   // entries per bucket before split
  float load_factor = 0.75f;
  std::uint32_t hash_seed = 0;
};

// ---------------------------------------------------------------------------
// Value encoding: [key_len: 4-byte LE u32][original_key][value]
// ---------------------------------------------------------------------------

inline auto encode_value(bytecask::BytesView key,
                         bytecask::BytesView value) -> bytecask::Bytes {
  auto key_len = static_cast<std::uint32_t>(key.size());
  bytecask::Bytes buf(4 + key.size() + value.size());
  std::memcpy(buf.data(), &key_len, 4);
  std::memcpy(buf.data() + 4, key.data(), key.size());
  std::memcpy(buf.data() + 4 + key.size(), value.data(), value.size());
  return buf;
}

struct DecodedValue {
  bytecask::BytesView original_key;
  bytecask::BytesView value;
};

inline auto decode_value(bytecask::BytesView encoded) -> DecodedValue {
  std::uint32_t key_len;
  std::memcpy(&key_len, encoded.data(), 4);
  return {
      encoded.subspan(4, key_len),
      encoded.subspan(4 + key_len),
  };
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

  void put(bytecask::BytesView key, bytecask::BytesView value) {
    auto bucket = route(key);
    auto prefix = make_bucket_prefix(bucket);
    auto ts = now_ts();
    auto fp = hash16(key);
    auto full_key = make_entry_key(prefix, ts, fp);

    auto encoded = encode_value(key, value);
    db_.put({.sync = false}, to_bv(full_key), bytecask::BytesView{encoded});
    ++entry_count_;

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
    auto prefix = make_bucket_prefix(bucket);
    auto target_fp = hash16(key);

    // Reverse keys-only scan: newest first, pure in-memory.
    auto next_prefix = make_bucket_prefix(bucket + 1);
    for (auto &rkey :
         db_.rkeys_from({.verify_checksums = false}, to_bv(next_prefix))) {
      auto key_span = key_obj_to_bv(rkey);
      if (!starts_with(key_span, to_bv(prefix)))
        break;

      // Fingerprint filter — in-memory, no disk I/O.
      auto fp = extract_hash16(key_span, prefix.size());
      if (fp != target_fp)
        continue;

      // Fingerprint match — fetch value from disk.
      bytecask::Bytes encoded;
      if (!db_.get({.verify_checksums = false}, key_span, encoded))
        continue;

      auto decoded = decode_value(bytecask::BytesView{encoded});
      if (!spans_equal(decoded.original_key, key))
        continue; // false positive (1/65536)

      // Latest match found (reverse scan = newest first).
      if (decoded.value.empty())
        return false; // tombstone
      out.assign(decoded.value.begin(), decoded.value.end());
      return true;
    }
    return false;
  }

  auto contains_key(bytecask::BytesView key) -> bool {
    auto bucket = route(key);
    auto prefix = make_bucket_prefix(bucket);
    auto target_fp = hash16(key);

    auto next_prefix = make_bucket_prefix(bucket + 1);
    for (auto &rkey :
         db_.rkeys_from({.verify_checksums = false}, to_bv(next_prefix))) {
      auto key_span = key_obj_to_bv(rkey);
      if (!starts_with(key_span, to_bv(prefix)))
        break;

      auto fp = extract_hash16(key_span, prefix.size());
      if (fp != target_fp)
        continue;

      bytecask::Bytes encoded;
      if (!db_.get({.verify_checksums = false}, key_span, encoded))
        continue;

      auto decoded = decode_value(bytecask::BytesView{encoded});
      if (!spans_equal(decoded.original_key, key))
        continue;

      return !decoded.value.empty();
    }
    return false;
  }

  void del(bytecask::BytesView key) {
    auto bucket = route(key);
    auto prefix = make_bucket_prefix(bucket);
    auto ts = now_ts();
    auto fp = hash16(key);
    auto full_key = make_entry_key(prefix, ts, fp);

    // Tombstone: encode with empty value portion.
    auto encoded = encode_value(key, {});
    db_.put({.sync = false}, to_bv(full_key), bytecask::BytesView{encoded});
    ++entry_count_;
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

  static void write_be32(char *dst, std::uint32_t v) {
    dst[0] = static_cast<char>((v >> 24) & 0xFF);
    dst[1] = static_cast<char>((v >> 16) & 0xFF);
    dst[2] = static_cast<char>((v >> 8) & 0xFF);
    dst[3] = static_cast<char>(v & 0xFF);
  }

  static void write_be16(char *dst, std::uint16_t v) {
    dst[0] = static_cast<char>((v >> 8) & 0xFF);
    dst[1] = static_cast<char>(v & 0xFF);
  }

  static auto read_be32(const std::byte *src) -> std::uint32_t {
    return (static_cast<std::uint32_t>(std::to_integer<uint8_t>(src[0])) << 24) |
           (static_cast<std::uint32_t>(std::to_integer<uint8_t>(src[1])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<uint8_t>(src[2])) << 8) |
           static_cast<std::uint32_t>(std::to_integer<uint8_t>(src[3]));
  }

  static auto read_be16(const std::byte *src) -> std::uint16_t {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(std::to_integer<uint8_t>(src[0])) << 8) |
        static_cast<std::uint16_t>(std::to_integer<uint8_t>(src[1])));
  }

  // Build bucket prefix: <ns>/b/<4-byte BE bucket_id>
  auto make_bucket_prefix(std::uint32_t bucket_id) const -> std::string {
    std::string result = bucket_prefix_;
    char be[4];
    write_be32(be, bucket_id);
    result.append(be, 4);
    return result;
  }

  // Build full entry key: <bucket_prefix><4B BE timestamp><2B BE hash16>
  static auto make_entry_key(const std::string &prefix,
                             std::uint32_t ts,
                             std::uint16_t fp) -> std::string {
    std::string result = prefix;
    char be[6];
    write_be32(be, ts);
    write_be16(be + 4, fp);
    result.append(be, 6);
    return result;
  }

  // Extract timestamp from a full entry key (4 bytes at prefix_len).
  static auto extract_ts(bytecask::BytesView full_key,
                         std::size_t prefix_len) -> std::uint32_t {
    return read_be32(full_key.data() + prefix_len);
  }

  // Extract hash16 from a full entry key (2 bytes at prefix_len + 4).
  static auto extract_hash16(bytecask::BytesView full_key,
                             std::size_t prefix_len) -> std::uint16_t {
    return read_be16(full_key.data() + prefix_len + 4);
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
    for (auto &key : db_.keys_from({}, to_bv(bucket_prefix_))) {
      auto key_span = key_obj_to_bv(key);
      if (!starts_with(key_span, to_bv(bucket_prefix_)))
        break;
      ++entry_count_;
    }
  }

  void split() {
    auto n = initial_size_ * (1u << round_);
    auto old_prefix = make_bucket_prefix(split_pointer_);

    // Phase 1: keys-only scan to collect all entry keys (in-memory, free).
    std::vector<std::string> all_old_keys;
    for (auto &key : db_.keys_from({}, to_bv(old_prefix))) {
      auto key_span = key_obj_to_bv(key);
      if (!starts_with(key_span, to_bv(old_prefix)))
        break;
      all_old_keys.emplace_back(
          reinterpret_cast<const char *>(key_span.data()), key_span.size());
    }

    if (all_old_keys.empty()) {
      advance_split_pointer(n);
      return;
    }

    // Phase 2: fetch values and deduplicate by original key (latest ts wins).
    struct BucketEntry {
      std::string old_full_key;
      bytecask::Bytes encoded_value;
      std::uint32_t ts;
    };
    std::map<std::string, BucketEntry> best_by_key;

    for (auto &old_key : all_old_keys) {
      bytecask::Bytes val;
      if (!db_.get({.verify_checksums = false}, to_bv(old_key), val))
        continue;

      auto decoded = decode_value(bytecask::BytesView{val});
      auto ts = extract_ts(to_bv(old_key), old_prefix.size());

      std::string orig_key_str(
          reinterpret_cast<const char *>(decoded.original_key.data()),
          decoded.original_key.size());

      auto it = best_by_key.find(orig_key_str);
      if (it == best_by_key.end() || ts >= it->second.ts) {
        best_by_key[orig_key_str] = {old_key, std::move(val), ts};
      }
    }

    // Phase 3: build atomic write plan.
    auto snap = db_.snapshot();
    bytecask::WritePlan plan{std::move(snap)};
    plan.ensure_unchanged(to_bv(meta_key_));

    // Delete ALL old entries in this bucket.
    for (auto &old_key : all_old_keys) {
      plan.del(to_bv(old_key));
    }

    // Re-insert surviving entries (skip tombstones) with fresh timestamps.
    for (auto &[orig_key_str, entry] : best_by_key) {
      auto decoded = decode_value(bytecask::BytesView{entry.encoded_value});

      // Skip tombstones — GC during split.
      if (decoded.value.empty())
        continue;

      auto h = murmur3_32(decoded.original_key.data(),
                          decoded.original_key.size(), opts_.hash_seed);
      auto new_bucket = h % (2 * n);
      auto fp = hash16(decoded.original_key);
      auto ts = now_ts();
      auto new_key = make_entry_key(make_bucket_prefix(new_bucket), ts, fp);

      plan.put(to_bv(new_key), bytecask::BytesView{entry.encoded_value});
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

  static auto now_ts() -> std::uint32_t {
    auto ns = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ns).count());
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
