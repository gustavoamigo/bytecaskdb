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
// Chain encoding: zero or more entries packed end-to-end.
//   entry = [key_len:4B LE][original_key:key_len][val_len:4B LE][user_value:val_len]
// val_len == 0 is a tombstone.
// ---------------------------------------------------------------------------

struct ChainEntry {
  std::vector<std::byte> original_key;
  std::vector<std::byte> user_value; // empty = tombstone
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
    auto fp = hash16(key);
    auto slot_key = make_entry_key(prefix, fp);

    auto new_entry = encode_chain_entry(key, value);
    bytecask::Bytes existing;
    if (!db_.get({.verify_checksums = false}, to_bv(slot_key), existing)) {
      // Slot absent — pure append (the common case).
      db_.put({.sync = false}, to_bv(slot_key), bytecask::BytesView{new_entry});
      ++entry_count_;
    } else {
      // Slot occupied — update matching entry or append a new one.
      auto entries = decode_chain(bytecask::BytesView{existing});
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
          new_chain.insert(new_chain.end(), reenc.begin(), reenc.end());
        }
      }
      if (!found) {
        new_chain.insert(new_chain.end(), new_entry.begin(), new_entry.end());
        ++entry_count_;
      }
      db_.put({.sync = false}, to_bv(slot_key), bytecask::BytesView{new_chain});
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
    auto prefix = make_bucket_prefix(bucket);
    auto fp = hash16(key);
    auto slot_key = make_entry_key(prefix, fp);

    // Single point-lookup — one disk read, no scan.
    bytecask::Bytes chain_bytes;
    if (!db_.get({.verify_checksums = false}, to_bv(slot_key), chain_bytes))
      return false;

    // Walk chain in memory to find the entry for this original_key.
    auto entries = decode_chain(bytecask::BytesView{chain_bytes});
    for (auto &e : entries) {
      if (!spans_equal(
              bytecask::BytesView{e.original_key.data(), e.original_key.size()},
              key))
        continue;
      if (e.user_value.empty())
        return false; // tombstone
      out.assign(e.user_value.begin(), e.user_value.end());
      return true;
    }
    return false;
  }

  auto contains_key(bytecask::BytesView key) -> bool {
    auto bucket = route(key);
    auto prefix = make_bucket_prefix(bucket);
    auto fp = hash16(key);
    auto slot_key = make_entry_key(prefix, fp);

    bytecask::Bytes chain_bytes;
    if (!db_.get({.verify_checksums = false}, to_bv(slot_key), chain_bytes))
      return false;

    auto entries = decode_chain(bytecask::BytesView{chain_bytes});
    for (auto &e : entries) {
      if (!spans_equal(
              bytecask::BytesView{e.original_key.data(), e.original_key.size()},
              key))
        continue;
      return !e.user_value.empty();
    }
    return false;
  }

  void del(bytecask::BytesView key) {
    auto bucket = route(key);
    auto prefix = make_bucket_prefix(bucket);
    auto fp = hash16(key);
    auto slot_key = make_entry_key(prefix, fp);

    auto tombstone = encode_chain_entry(key, {});
    bytecask::Bytes existing;
    if (!db_.get({.verify_checksums = false}, to_bv(slot_key), existing)) {
      db_.put({.sync = false}, to_bv(slot_key), bytecask::BytesView{tombstone});
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
      db_.put({.sync = false}, to_bv(slot_key), bytecask::BytesView{new_chain});
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


  auto make_bucket_prefix(std::uint32_t bucket_id) const -> std::string {
    std::string result = bucket_prefix_;
    char be[4];
    write_be32(be, bucket_id);
    result.append(be, 4);
    return result;
  }

  // Build slot key: <bucket_prefix><2B BE hash16>
  static auto make_entry_key(const std::string &prefix,
                             std::uint16_t fp) -> std::string {
    std::string result = prefix;
    char be[2];
    write_be16(be, fp);
    result.append(be, 2);
    return result;
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

    // Phase 1: keys-only scan to collect all slot keys (in-memory, free).
    std::vector<std::string> old_slot_keys;
    for (auto &key : db_.keys_from({}, to_bv(old_prefix))) {
      auto key_span = key_obj_to_bv(key);
      if (!starts_with(key_span, to_bv(old_prefix)))
        break;
      old_slot_keys.emplace_back(
          reinterpret_cast<const char *>(key_span.data()), key_span.size());
    }

    if (old_slot_keys.empty()) {
      advance_split_pointer(n);
      return;
    }

    // Phase 2: decode all chains, deduplicate by original_key (last write wins).
    std::map<std::string, ChainEntry> survivors;
    for (auto &slot_key : old_slot_keys) {
      bytecask::Bytes chain_bytes;
      if (!db_.get({.verify_checksums = false}, to_bv(slot_key), chain_bytes))
        continue;
      auto entries = decode_chain(bytecask::BytesView{chain_bytes});
      for (auto &e : entries) {
        std::string k(reinterpret_cast<const char *>(e.original_key.data()),
                      e.original_key.size());
        survivors[k] = std::move(e);
      }
    }

    // Phase 3: build atomic write plan.
    auto snap = db_.snapshot();
    bytecask::WritePlan plan{std::move(snap)};
    plan.ensure_unchanged(to_bv(meta_key_));

    for (auto &slot_key : old_slot_keys) {
      plan.del(to_bv(slot_key));
    }

    // Group surviving (non-tombstone) entries by their new slot key.
    std::map<std::string, bytecask::Bytes> new_chains;
    for (auto &[orig_key_str, entry] : survivors) {
      if (entry.user_value.empty())
        continue; // tombstone — GC during split

      auto orig_bv = bytecask::BytesView{entry.original_key.data(),
                                        entry.original_key.size()};
      auto h = murmur3_32(orig_bv.data(), orig_bv.size(), opts_.hash_seed);
      auto new_bucket = h % (2 * n);
      auto fp = hash16(orig_bv);
      auto new_slot_key = make_entry_key(make_bucket_prefix(new_bucket), fp);

      auto entry_bytes = encode_chain_entry(
          orig_bv,
          bytecask::BytesView{entry.user_value.data(), entry.user_value.size()});
      auto &chain = new_chains[new_slot_key];
      chain.insert(chain.end(), entry_bytes.begin(), entry_bytes.end());
    }

    for (auto &[new_slot_key, chain] : new_chains) {
      plan.put(to_bv(new_slot_key),
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
