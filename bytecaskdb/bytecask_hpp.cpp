// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_hpp.cpp — PIMPL implementation for include/bytecask.hpp.
//
// This is the only translation unit that imports the bytecask module. It
// defines all nested Impl structs and out-of-line method bodies for the
// bytecask::internal PIMPL wrapper classes declared in include/bytecask.hpp.

// Standard library headers must be included BEFORE the module import.
//
// On macOS, libc++ marks many inline/template functions with
// _LIBCPP_HIDE_FROM_ABI, which expands to __attribute__((__abi_tag__("..."))).
// That tag changes the mangled name so different libc++ versions don't clash
// at link time.
//
// When `import bytecask;` is processed, the compiler loads the module's BMI
// and resolves exported types that reference std types (e.g., ReadOptions
// holds std::chrono::milliseconds). On LLVM 22+, this causes those std
// declarations to become "known" in the importing TU. A subsequent
// `#include <chrono>` then tries to re-declare the same entities and add
// __abi_tag__ attributes to declarations that already exist — which LLVM 22
// rejects as a hard error: "cannot add 'abi_tag' attribute in a redeclaration".
//
// Including std headers first establishes their canonical declarations in this
// TU before the module import. The import then finds them already present and
// consistent — no conflict.
//
// BYTECASK_HPP_NO_STD_INCLUDES tells bytecask.hpp to skip its own copies of
// these includes since they are already in scope.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Import the module — populates bytecask:: namespace with all types.
import bytecask;

// BYTECASK_HPP_IMPL_MODE suppresses the bytecask:: namespace aliases
// in the header that would conflict with the module-imported types. In this
// mode the header only defines the bytecask::internal:: types (plain types +
// PIMPL wrapper classes). Method signatures use bytecask::internal:: types
// exclusively, so mangling is consistent regardless of how the header is consumed.
//
// BYTECASK_HPP_NO_STD_INCLUDES tells the header to skip its own std includes
// since we already included them above (before the module import).
#define BYTECASK_HPP_IMPL_MODE
#define BYTECASK_HPP_NO_STD_INCLUDES
#include "../include/bytecask.hpp"

// ---------------------------------------------------------------------------
// Local conversion helpers — bytecask::internal:: <-> bytecask:: (module) types.
//
// Only needed for struct/enum types that get C++23 module attachment when
// imported via `import bytecask;`. Type aliases (Bytes, BytesView) map to
// std:: types that are never module-attached, so they convert implicitly.
// ---------------------------------------------------------------------------

namespace {

auto to_module(bytecask::internal::WriteOptions o) noexcept -> bytecask::WriteOptions {
  return {o.sync, o.solo};
}

auto to_module(const bytecask::internal::ReadOptions& o) noexcept -> bytecask::ReadOptions {
  return {o.staleness_tolerance, o.verify_checksums};
}

auto to_module(bytecask::internal::VacuumOptions o) noexcept -> bytecask::VacuumOptions {
  return {o.fragmentation_threshold};
}

auto to_module(bytecask::internal::Options o) noexcept -> bytecask::Options {
  return {
    o.max_file_bytes,
    o.recovery_threads,
    o.fail_recovery_on_crc_errors,
    static_cast<bytecask::Mode>(o.initial_mode),
    o.max_key_bytes,
    o.max_value_bytes,
  };
}

auto to_module(bytecask::internal::Mode m) noexcept -> bytecask::Mode {
  return static_cast<bytecask::Mode>(m);
}

auto from_module(bytecask::Mode m) noexcept -> bytecask::internal::Mode {
  return static_cast<bytecask::internal::Mode>(m);
}

auto to_module(const bytecask::internal::DataEntryView& v) noexcept
    -> bytecask::DataEntryView {
  return {v.sequence, static_cast<bytecask::EntryType>(v.entry_type), v.key, v.value};
}

auto from_module(const bytecask::DataEntryView& v) noexcept
    -> bytecask::internal::DataEntryView {
  return {v.sequence, static_cast<bytecask::internal::EntryType>(v.entry_type), v.key, v.value};
}

// Translate module-attached exception types to the header-defined internal types.
// The module's bytecask::DbDegraded and bytecask::DbFollowerMode carry the
// C++23 module attachment suffix in their mangled names, so they cannot be
// caught by the header-defined internal types at call sites that only see the header.
template <typename F>
auto translate_exceptions(F&& f) -> decltype(std::forward<F>(f)()) {
  try {
    return std::forward<F>(f)();
  } catch (const bytecask::DbDegraded& e) {
    throw bytecask::internal::DbDegraded(e.what());
  } catch (const bytecask::DbFollowerMode& e) {
    throw bytecask::internal::DbFollowerMode(e.what());
  }
}

} // namespace

namespace bytecask::internal {

// ---------------------------------------------------------------------------
// KeyIterator::Impl
// ---------------------------------------------------------------------------

struct KeyIterator::Impl {
  bytecask::KeyIterator cur;
  mutable Bytes cached;
  mutable bool cached_valid{false};

  explicit Impl(bytecask::KeyIterator c) : cur{std::move(c)} {}

  auto get() const -> const Bytes& {
    if (!cached_valid) {
      if (cur != std::default_sentinel) {
        const auto& k = *cur;
        cached.assign(k.begin(), k.end());
      }
      cached_valid = true;
    }
    return cached;
  }
};

KeyIterator::KeyIterator() noexcept = default;
KeyIterator::~KeyIterator() = default;
KeyIterator::KeyIterator(KeyIterator&&) noexcept = default;
KeyIterator& KeyIterator::operator=(KeyIterator&&) noexcept = default;
KeyIterator::KeyIterator(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

auto KeyIterator::operator*() const -> const Bytes& {
  return impl_->get();
}

auto KeyIterator::operator++() -> KeyIterator& {
  ++impl_->cur;
  impl_->cached_valid = false;
  return *this;
}

void KeyIterator::operator++(int) { ++*this; }

auto KeyIterator::operator==(std::default_sentinel_t) const noexcept -> bool {
  return !impl_ || (impl_->cur == std::default_sentinel);
}

// ---------------------------------------------------------------------------
// EntryIterator::Impl
// ---------------------------------------------------------------------------

struct EntryIterator::Impl {
  bytecask::EntryIterator cur;
  mutable std::pair<Bytes, Bytes> cached;
  mutable bool cached_valid{false};

  explicit Impl(bytecask::EntryIterator c) : cur{std::move(c)} {}

  auto get() const -> const std::pair<Bytes, Bytes>& {
    if (!cached_valid) {
      if (cur != std::default_sentinel) {
        const auto& [k, v] = *cur;
        cached.first.assign(k.begin(), k.end());
        cached.second = v;
      }
      cached_valid = true;
    }
    return cached;
  }
};

EntryIterator::EntryIterator() noexcept = default;
EntryIterator::~EntryIterator() = default;
EntryIterator::EntryIterator(EntryIterator&&) noexcept = default;
EntryIterator& EntryIterator::operator=(EntryIterator&&) noexcept = default;
EntryIterator::EntryIterator(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

auto EntryIterator::operator*() const
    -> const std::pair<Bytes, Bytes>& {
  return impl_->get();
}

auto EntryIterator::operator++() -> EntryIterator& {
  ++impl_->cur;
  impl_->cached_valid = false;
  return *this;
}

void EntryIterator::operator++(int) { ++*this; }

auto EntryIterator::operator==(std::default_sentinel_t) const noexcept -> bool {
  return !impl_ || (impl_->cur == std::default_sentinel);
}

// ---------------------------------------------------------------------------
// ReverseKeyIterator::Impl
//
// Copyable so that ReverseKeyIterator satisfies semiregular — required when
// it is used as both iterator and sentinel in
// subrange<ReverseKeyIterator, ReverseKeyIterator>.
// ---------------------------------------------------------------------------

struct ReverseKeyIterator::Impl {
  bytecask::ReverseKeyIterator cur;
  mutable Bytes cached;
  mutable bool cached_valid{false};

  explicit Impl(bytecask::ReverseKeyIterator c) : cur{std::move(c)} {}
  Impl(const Impl& o)
      : cur{o.cur}, cached{o.cached}, cached_valid{o.cached_valid} {}

  auto get() const -> const Bytes& {
    if (!cached_valid) {
      const auto& k = *cur;
      cached.assign(k.begin(), k.end());
      cached_valid = true;
    }
    return cached;
  }
};

ReverseKeyIterator::ReverseKeyIterator() noexcept = default;
ReverseKeyIterator::~ReverseKeyIterator() = default;
ReverseKeyIterator::ReverseKeyIterator(ReverseKeyIterator&&) noexcept = default;
ReverseKeyIterator& ReverseKeyIterator::operator=(ReverseKeyIterator&&) noexcept = default;
ReverseKeyIterator::ReverseKeyIterator(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

ReverseKeyIterator::ReverseKeyIterator(const ReverseKeyIterator& other)
    : impl_{other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr} {}

ReverseKeyIterator& ReverseKeyIterator::operator=(const ReverseKeyIterator& other) {
  if (this != &other) {
    impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
  }
  return *this;
}

auto ReverseKeyIterator::operator*() const -> const Bytes& {
  return impl_->get();
}

auto ReverseKeyIterator::operator++() -> ReverseKeyIterator& {
  ++impl_->cur;
  impl_->cached_valid = false;
  return *this;
}

void ReverseKeyIterator::operator++(int) { ++*this; }

auto ReverseKeyIterator::operator==(const ReverseKeyIterator& other) const noexcept -> bool {
  if (!impl_ && !other.impl_) return true;
  if (!impl_ || !other.impl_) return false;
  return impl_->cur == other.impl_->cur;
}

// ---------------------------------------------------------------------------
// ReverseEntryIterator::Impl — copyable for the same reason as ReverseKeyIterator
// ---------------------------------------------------------------------------

struct ReverseEntryIterator::Impl {
  bytecask::ReverseEntryIterator cur;
  mutable std::pair<Bytes, Bytes> cached;
  mutable bool cached_valid{false};

  explicit Impl(bytecask::ReverseEntryIterator c) : cur{std::move(c)} {}
  Impl(const Impl& o)
      : cur{o.cur}, cached{o.cached}, cached_valid{o.cached_valid} {}

  auto get() const -> const std::pair<Bytes, Bytes>& {
    if (!cached_valid) {
      const auto& [k, v] = *cur;
      cached.first.assign(k.begin(), k.end());
      cached.second = v;
      cached_valid = true;
    }
    return cached;
  }
};

ReverseEntryIterator::ReverseEntryIterator() noexcept = default;
ReverseEntryIterator::~ReverseEntryIterator() = default;
ReverseEntryIterator::ReverseEntryIterator(ReverseEntryIterator&&) noexcept = default;
ReverseEntryIterator& ReverseEntryIterator::operator=(ReverseEntryIterator&&) noexcept = default;
ReverseEntryIterator::ReverseEntryIterator(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

ReverseEntryIterator::ReverseEntryIterator(const ReverseEntryIterator& other)
    : impl_{other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr} {}

ReverseEntryIterator& ReverseEntryIterator::operator=(const ReverseEntryIterator& other) {
  if (this != &other) {
    impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
  }
  return *this;
}

auto ReverseEntryIterator::operator*() const
    -> const std::pair<Bytes, Bytes>& {
  return impl_->get();
}

auto ReverseEntryIterator::operator++() -> ReverseEntryIterator& {
  ++impl_->cur;
  impl_->cached_valid = false;
  return *this;
}

void ReverseEntryIterator::operator++(int) { ++*this; }

auto ReverseEntryIterator::operator==(const ReverseEntryIterator& other) const noexcept -> bool {
  if (!impl_ && !other.impl_) return true;
  if (!impl_ || !other.impl_) return false;
  return impl_->cur == other.impl_->cur;
}

// ---------------------------------------------------------------------------
// ChangeIterator::Impl
// ---------------------------------------------------------------------------

struct ChangeIterator::Impl {
  bytecask::ChangeIterator cur;
  mutable DataEntryView cached{};
  mutable bool cached_valid{false};

  explicit Impl(bytecask::ChangeIterator c) : cur{std::move(c)} {}

  auto get() const -> const DataEntryView& {
    if (!cached_valid) {
      if (cur != std::default_sentinel) {
        cached = from_module(*cur);
      }
      cached_valid = true;
    }
    return cached;
  }
};

ChangeIterator::ChangeIterator() noexcept = default;
ChangeIterator::~ChangeIterator() = default;
ChangeIterator::ChangeIterator(ChangeIterator&&) noexcept = default;
ChangeIterator& ChangeIterator::operator=(ChangeIterator&&) noexcept = default;
ChangeIterator::ChangeIterator(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

auto ChangeIterator::operator*() const -> const DataEntryView& {
  return impl_->get();
}

auto ChangeIterator::operator++() -> ChangeIterator& {
  ++impl_->cur;
  impl_->cached_valid = false;
  return *this;
}

void ChangeIterator::operator++(int) {
  ++impl_->cur;
  impl_->cached_valid = false;
}

auto ChangeIterator::operator==(std::default_sentinel_t) const noexcept -> bool {
  return !impl_ || (impl_->cur == std::default_sentinel);
}

// ---------------------------------------------------------------------------
// Snapshot::Impl
// ---------------------------------------------------------------------------

struct Snapshot::Impl {
  bytecask::Snapshot snap;
  explicit Impl(bytecask::Snapshot s) : snap{std::move(s)} {}
};

Snapshot::~Snapshot() = default;
Snapshot::Snapshot(Snapshot&&) noexcept = default;
Snapshot& Snapshot::operator=(Snapshot&&) noexcept = default;
Snapshot::Snapshot(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

auto Snapshot::get(const ReadOptions& opts,
                   BytesView key,
                   Bytes& out) const -> bool {
  return impl_->snap.get(to_module(opts), key, out);
}

auto Snapshot::contains_key(const ReadOptions& opts,
                             BytesView key) const -> bool {
  return impl_->snap.contains_key(to_module(opts), key);
}

auto Snapshot::iter_from(const ReadOptions& opts,
                         BytesView from) const
    -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
  auto r = impl_->snap.iter_from(to_module(opts), from);
  return {EntryIterator{std::make_unique<EntryIterator::Impl>(r.begin())},
          std::default_sentinel};
}

auto Snapshot::keys_from(const ReadOptions& opts,
                         BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto r = impl_->snap.keys_from(to_module(opts), from);
  return {KeyIterator{std::make_unique<KeyIterator::Impl>(r.begin())},
          std::default_sentinel};
}

auto Snapshot::riter_from(const ReadOptions& opts,
                          BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator> {
  auto r = impl_->snap.riter_from(to_module(opts), from);
  return {
    ReverseEntryIterator{std::make_unique<ReverseEntryIterator::Impl>(r.begin())},
    ReverseEntryIterator{std::make_unique<ReverseEntryIterator::Impl>(r.end())}
  };
}

auto Snapshot::rkeys_from(const ReadOptions& opts,
                          BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto r = impl_->snap.rkeys_from(to_module(opts), from);
  return {
    ReverseKeyIterator{std::make_unique<ReverseKeyIterator::Impl>(r.begin())},
    ReverseKeyIterator{std::make_unique<ReverseKeyIterator::Impl>(r.end())}
  };
}

// ---------------------------------------------------------------------------
// WritePlan::Impl
// ---------------------------------------------------------------------------

struct WritePlan::Impl {
  std::optional<bytecask::WritePlan> plan;
  explicit Impl() : plan{std::in_place} {}
  explicit Impl(bytecask::WritePlan p) : plan{std::move(p)} {}
};

WritePlan::WritePlan() : impl_{std::make_unique<Impl>()} {}

WritePlan::WritePlan(Snapshot snap)
    : impl_{std::make_unique<Impl>(
          bytecask::WritePlan{std::move(snap.impl_->snap)})} {}

WritePlan::~WritePlan() = default;
WritePlan::WritePlan(WritePlan&&) noexcept = default;
WritePlan& WritePlan::operator=(WritePlan&&) noexcept = default;

void WritePlan::put(BytesView key, BytesView value) {
  impl_->plan->put(key, value);
}

void WritePlan::del(BytesView key) {
  impl_->plan->del(key);
}

void WritePlan::del_range(BytesView from, BytesView to) {
  impl_->plan->del_range(from, to);
}

void WritePlan::ensure_present(BytesView key) {
  impl_->plan->ensure_present(key);
}

void WritePlan::ensure_absent(BytesView key) {
  impl_->plan->ensure_absent(key);
}

void WritePlan::ensure_unchanged(BytesView key) {
  impl_->plan->ensure_unchanged(key);
}

void WritePlan::ensure_range_unchanged(BytesView from,
                                       BytesView to) {
  impl_->plan->ensure_range_unchanged(from, to);
}

auto WritePlan::has_snapshot() const noexcept -> bool {
  return impl_->plan && impl_->plan->has_snapshot();
}

// ---------------------------------------------------------------------------
// DB::Impl — uses guaranteed copy elision (C++17) to heap-allocate the
// non-moveable bytecask::DB.
// ---------------------------------------------------------------------------

struct DB::Impl {
  bytecask::DB db;
  explicit Impl(std::filesystem::path dir, Options opts)
      : db{bytecask::DB::open(std::move(dir), to_module(opts))} {}
};

DB::DB(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
DB::~DB() = default;

auto DB::open(std::filesystem::path dir, Options opts) -> DB {
  return DB{std::make_unique<Impl>(std::move(dir), std::move(opts))};
}

auto DB::get(const ReadOptions& opts,
             BytesView key,
             Bytes& out) const -> bool {
  return translate_exceptions([&] {
    return impl_->db.get(to_module(opts), key, out);
  });
}

void DB::put(const WriteOptions& opts,
             BytesView key, BytesView value) {
  translate_exceptions([&] {
    impl_->db.put(to_module(opts), key, value);
  });
}

auto DB::del(const WriteOptions& opts,
             BytesView key) -> bool {
  return translate_exceptions([&] {
    return impl_->db.del(to_module(opts), key);
  });
}

void DB::del_range(const WriteOptions& opts,
                   BytesView from, BytesView to) {
  translate_exceptions([&] {
    impl_->db.del_range(to_module(opts), from, to);
  });
}

auto DB::contains_key(const ReadOptions& opts,
                      BytesView key) const -> bool {
  return impl_->db.contains_key(to_module(opts), key);
}

auto DB::mode() const noexcept -> Mode {
  return from_module(impl_->db.mode());
}

void DB::set_mode(Mode mode) {
  impl_->db.set_mode(to_module(mode));
}

auto DB::is_degraded() const noexcept -> bool {
  return impl_->db.is_degraded();
}

auto DB::degraded_reason() const noexcept -> std::string {
  return impl_->db.degraded_reason();
}

void DB::resume() {
  impl_->db.resume();
}

auto DB::snapshot() const -> Snapshot {
  return Snapshot{std::make_unique<Snapshot::Impl>(impl_->db.snapshot())};
}

auto DB::apply_batch(WriteOptions opts, WritePlan plan) -> bool {
  return translate_exceptions([&] {
    return impl_->db.apply_batch(to_module(opts), std::move(*plan.impl_->plan));
  });
}

auto DB::iter_from(const ReadOptions& opts,
                   BytesView from) const
    -> std::ranges::subrange<EntryIterator, std::default_sentinel_t> {
  auto r = impl_->db.iter_from(to_module(opts), from);
  return {EntryIterator{std::make_unique<EntryIterator::Impl>(r.begin())},
          std::default_sentinel};
}

auto DB::keys_from(const ReadOptions& opts,
                   BytesView from) const
    -> std::ranges::subrange<KeyIterator, std::default_sentinel_t> {
  auto r = impl_->db.keys_from(to_module(opts), from);
  return {KeyIterator{std::make_unique<KeyIterator::Impl>(r.begin())},
          std::default_sentinel};
}

auto DB::riter_from(const ReadOptions& opts,
                    BytesView from) const
    -> std::ranges::subrange<ReverseEntryIterator, ReverseEntryIterator> {
  auto r = impl_->db.riter_from(to_module(opts), from);
  return {
    ReverseEntryIterator{std::make_unique<ReverseEntryIterator::Impl>(r.begin())},
    ReverseEntryIterator{std::make_unique<ReverseEntryIterator::Impl>(r.end())}
  };
}

auto DB::rkeys_from(const ReadOptions& opts,
                    BytesView from) const
    -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator> {
  auto r = impl_->db.rkeys_from(to_module(opts), from);
  return {
    ReverseKeyIterator{std::make_unique<ReverseKeyIterator::Impl>(r.begin())},
    ReverseKeyIterator{std::make_unique<ReverseKeyIterator::Impl>(r.end())}
  };
}

auto DB::vacuum(VacuumOptions opts) -> bool {
  return impl_->db.vacuum(to_module(opts));
}

auto DB::current_sequence(std::chrono::milliseconds timeout) const
    -> std::uint64_t {
  return impl_->db.current_sequence(timeout);
}

auto DB::create_manifest() -> FileManifest {
  auto m = impl_->db.create_manifest();
  std::vector<FileInfo> files;
  files.reserve(m.files.size());
  for (const auto& fi : m.files) {
    files.push_back({fi.file_id, fi.data_path, fi.hint_path});
  }
  return FileManifest{
    Snapshot{std::make_unique<Snapshot::Impl>(std::move(m.snap))},
    std::move(files),
    m.through_sequence
  };
}

auto DB::changes_since(const Snapshot& snap, std::uint64_t from_sequence) const
    -> std::ranges::subrange<ChangeIterator, std::default_sentinel_t> {
  auto r = impl_->db.changes_since(snap.impl_->snap, from_sequence);
  return {
    ChangeIterator{std::make_unique<ChangeIterator::Impl>(std::move(r).begin())},
    std::default_sentinel
  };
}

void DB::ingest(std::span<const DataEntryView> entries) {
  std::vector<bytecask::DataEntryView> module_entries;
  module_entries.reserve(entries.size());
  for (const auto& e : entries) {
    module_entries.push_back(to_module(e));
  }
  translate_exceptions([&] {
    impl_->db.ingest(module_entries);
  });
}

auto DB::stats() const -> std::map<std::string, std::int64_t> {
  return impl_->db.stats();
}

} // namespace bytecask::internal
