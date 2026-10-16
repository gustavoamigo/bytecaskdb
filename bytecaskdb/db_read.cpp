// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// ByteCaskDB — snapshot reads and range iteration

module;
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <system_error>
#include <utility>

module bytecask;

namespace bytecask {

auto Snapshot::contains_key(const ReadOptions& opts,
                            BytesView key) const -> bool {
  return kd_contains(state_->key_dir, key,
                     state_->kd_ctx(opts.verify_checksums));
}

// Reads the value for key from the frozen snapshot state into out.
auto Snapshot::get(const ReadOptions& opts, BytesView key,
                   Bytes &out) const -> bool {
#ifdef BYTECASK_KEYDIR_BLIND
  return kd_read_value(state_->key_dir, key,
                       state_->kd_ctx(opts.verify_checksums), out);
#else
  const auto kv = kd_get(state_->key_dir, key, state_->kd_ctx());
  if (!kv) return false;
  if (kv->value_size() == 0) {
    out.clear();
    return true;
  }
  ScratchUse scratch{value_read_scratch()};
  (*state_->files.get(kv->file_id()))
      ->read_value(kv->file_offset(), narrow<std::uint16_t>(key.size()),
                   kv->value_size(), opts.verify_checksums, scratch.buf(), out);
  return true;
#endif
}

auto Snapshot::iter_from(const ReadOptions& opts, BytesView from) const
    -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
  auto it = kd_value_lower_bound(state_->key_dir, from, state_->kd_ctx());
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{state_, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto Snapshot::keys_from(const ReadOptions& /*opts*/, BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto it = from.empty() ? kd_begin(state_->key_dir, state_->kd_ctx())
                         : kd_lower_bound(state_->key_dir, from,
                                          state_->kd_ctx());
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto Snapshot::riter_from(const ReadOptions& opts, BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t> {
  auto it = kd_value_rlower_bound(state_->key_dir, from, state_->kd_ctx());
  return std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>{
      ReverseEntryIterator{state_, std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto Snapshot::count_keys(BytesView from, BytesView to,
                          std::size_t limit) const -> std::size_t {
  check_range(from, to);
  if (limit == 0) return 0;
  return kd_count(state_->key_dir, from, to, limit, state_->kd_ctx());
}

auto Snapshot::rkeys_from(const ReadOptions& /*opts*/, BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto begin_it = from.empty()
      ? kd_end(state_->key_dir, state_->kd_ctx())
      : kd_upper_bound(state_->key_dir, from, state_->kd_ctx());
  auto end_it = kd_begin(state_->key_dir, state_->kd_ctx());
  return {ReverseKeyIterator{KeyIterator{std::move(begin_it)}},
          ReverseKeyIterator{KeyIterator{std::move(end_it)}}};
}

// Returns an input range of (key, value) pairs with keys >= from.
// Pass an empty span to start from the first key. Each dereference reads
// one value from disk via a single pread (lazy). Results are in ascending
// key order.
// Throws std::system_error on I/O failure.
auto DB::iter_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
  auto s = load_state_for_read();
  auto it = kd_value_lower_bound(s->key_dir, from, s->kd_ctx());
  return std::ranges::subrange<EntryIterator, std::default_sentinel_t>{
      EntryIterator{s.state(), std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

// Returns an input range of keys >= from. Reads no values; a key directory
// that holds no key bytes reads each key's record as the range advances.
auto DB::keys_from(const ReadOptions & /*opts*/, BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto s = load_state_for_read();
  auto it = from.empty() ? kd_begin(s->key_dir, s->kd_ctx())
                         : kd_lower_bound(s->key_dir, from, s->kd_ctx());
  return std::ranges::subrange<KeyIterator, std::default_sentinel_t>{
      KeyIterator{std::move(it)}, std::default_sentinel};
}

auto DB::riter_from(const ReadOptions &opts, BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t> {
  auto s = load_state_for_read();
  auto it = kd_value_rlower_bound(s->key_dir, from, s->kd_ctx());
  return std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>{
      ReverseEntryIterator{s.state(), std::move(it), opts.verify_checksums},
      std::default_sentinel};
}

auto DB::rkeys_from(const ReadOptions & /*opts*/, BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto s = load_state_for_read();
  auto begin_it = from.empty()
      ? kd_end(s->key_dir, s->kd_ctx())
      : kd_upper_bound(s->key_dir, from, s->kd_ctx());
  auto end_it = kd_begin(s->key_dir, s->kd_ctx());
  return {ReverseKeyIterator{KeyIterator{std::move(begin_it)}},
          ReverseKeyIterator{KeyIterator{std::move(end_it)}}};
}

} // namespace bytecask
