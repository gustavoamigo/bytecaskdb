// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask.hpp — public C++ header for ByteCaskDB.
//
// Exposes the full ByteCaskDB API using the PIMPL idiom so callers do not
// need to import the C++23 module or depend on any internal headers.
//
// All types — plain structs, enums, and PIMPL wrapper classes — live in
// namespace bytecask::internal. When included normally (not by the
// implementation file that imports the module), matching aliases in namespace
// bytecask are also defined so callers can write bytecask::DB,
// bytecask::WriteOptions, etc.
//
// The implementation file (bytecaskdb/bytecask_hpp.cpp) defines
// BYTECASK_HPP_IMPL_MODE before including this header to suppress the
// bytecask-namespace aliases that would conflict with the module-imported types.
// All plain types and PIMPL classes remain visible regardless of that flag.

#pragma once

// bytecask_hpp.cpp includes all std headers before `import bytecask;` to avoid
// an LLVM 22+ hard error (see that file for the full explanation), then defines
// this macro so we don't re-include headers that are already in scope.
#ifndef BYTECASK_HPP_NO_STD_INCLUDES
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#endif // BYTECASK_HPP_NO_STD_INCLUDES

// ---------------------------------------------------------------------------
// bytecask::internal namespace — all public types, always defined.
//
// Plain types (Bytes, WriteOptions, …) live here unconditionally so that
// PIMPL method signatures are always resolved against header-defined types,
// never against module-imported types. This prevents C++23 module-attachment
// differences from causing symbol-name mismatches at link time.
// ---------------------------------------------------------------------------

namespace bytecask::internal {

// ---------------------------------------------------------------------------
// Plain types — identical in layout and semantics to the module exports.
// ---------------------------------------------------------------------------

using Bytes     = std::vector<std::byte>;
using BytesView = std::span<const std::byte>;

enum class Mode { Leader, Follower };

enum class EntryType : std::uint8_t {
  Put       = 0x01,
  Delete    = 0x02,
  BulkBegin = 0x03,
  BulkEnd   = 0x04,
  RangeDel  = 0x05,
};

struct WriteOptions {
  bool sync{true};
  bool solo{false};
};

struct ReadOptions {
  std::chrono::milliseconds staleness_tolerance{0};
  bool verify_checksums{true};
};

struct VacuumOptions {
  double fragmentation_threshold{0.5};
};

struct Options {
  std::uint64_t max_file_bytes{64ULL * 1024 * 1024};
  unsigned recovery_threads{4};
  bool fail_recovery_on_crc_errors{true};
  Mode initial_mode{Mode::Leader};
  std::uint32_t max_key_bytes{4096};
  std::uint32_t max_value_bytes{4U * 1024 * 1024};
  bool use_mmap{false};
};

struct SizeLimits {
  std::uint32_t max_key_bytes{4096};
  std::uint32_t max_value_bytes{4U * 1024 * 1024};
};

struct FileInfo {
  std::uint32_t file_id;
  std::filesystem::path data_path;
  std::filesystem::path hint_path;
};

// Non-owning view of a raw committed entry. key/value spans are valid only
// until the iterator advances.
struct DataEntryView {
  std::uint64_t sequence;
  EntryType entry_type;
  std::span<const std::byte> key;
  std::span<const std::byte> value;
};

// Non-owning view of a key/value pair. Spans are valid until the iterator
// that produced them advances.
struct EntryView {
  std::span<const std::byte> key;
  std::span<const std::byte> value;
};

// Thrown by write operations when the engine is degraded. Reads remain
// available. Call DB::resume() for in-process recovery.
//
// The destructor is intentionally inline so the vtable is emitted per TU.
// This avoids a duplicate-symbol link conflict with the module's own vtable.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wweak-vtables"
class DbDegraded : public std::runtime_error {
public:
  explicit DbDegraded(const std::string& reason)
      : std::runtime_error(reason) {}
  DbDegraded(const DbDegraded&) = default;
  DbDegraded& operator=(const DbDegraded&) = default;
  ~DbDegraded() override = default;
};

// Thrown by put/del/apply_batch when the engine is in Follower mode.
class DbFollowerMode : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
  DbFollowerMode(const DbFollowerMode&) = default;
  DbFollowerMode& operator=(const DbFollowerMode&) = default;
  ~DbFollowerMode() override = default;
};
#pragma clang diagnostic pop

// ---------------------------------------------------------------------------
// PIMPL wrapper classes — forward-declare Impl; method signatures use only
// bytecask::internal:: types (defined above) so mangling is consistent across TUs.
// ---------------------------------------------------------------------------

// Forward declarations (used in return types before full class definitions).
class Snapshot;
class WritePlan;

// ---------------------------------------------------------------------------
// KeyIterator — ascending key walk (in-memory, no disk I/O)
// ---------------------------------------------------------------------------

class KeyIterator {
public:
  struct Impl;

  using iterator_concept = std::input_iterator_tag;
  using value_type       = Bytes;
  using difference_type  = std::ptrdiff_t;

  KeyIterator() noexcept;
  ~KeyIterator();
  KeyIterator(const KeyIterator&) = delete;
  KeyIterator& operator=(const KeyIterator&) = delete;
  KeyIterator(KeyIterator&&) noexcept;
  KeyIterator& operator=(KeyIterator&&) noexcept;

  auto operator*() const -> const Bytes&;
  auto operator++() -> KeyIterator&;
  void operator++(int);
  auto operator==(std::default_sentinel_t) const noexcept -> bool;

private:
  explicit KeyIterator(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class DB;
  friend class Snapshot;
};

// ---------------------------------------------------------------------------
// EntryIterator — ascending (key, value) walk; values fetched lazily from disk
// ---------------------------------------------------------------------------

class EntryIterator {
public:
  struct Impl;

  using iterator_concept = std::input_iterator_tag;
  using value_type       = EntryView;
  using difference_type  = std::ptrdiff_t;

  EntryIterator() noexcept;
  ~EntryIterator();
  EntryIterator(const EntryIterator&) = delete;
  EntryIterator& operator=(const EntryIterator&) = delete;
  EntryIterator(EntryIterator&&) noexcept;
  EntryIterator& operator=(EntryIterator&&) noexcept;

  auto operator*() const -> const EntryView&;
  auto operator++() -> EntryIterator&;
  void operator++(int);
  auto operator==(std::default_sentinel_t) const noexcept -> bool;

private:
  explicit EntryIterator(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class DB;
  friend class Snapshot;
};

// ---------------------------------------------------------------------------
// ReverseKeyIterator — descending key walk.
//
// Copyable: required because DB::rkeys_from returns
// subrange<ReverseKeyIterator, ReverseKeyIterator> (sentinel = same type).
// ---------------------------------------------------------------------------

class ReverseKeyIterator {
public:
  struct Impl;

  using iterator_concept = std::input_iterator_tag;
  using value_type       = Bytes;
  using difference_type  = std::ptrdiff_t;

  ReverseKeyIterator() noexcept;
  ~ReverseKeyIterator();
  ReverseKeyIterator(const ReverseKeyIterator& other);
  ReverseKeyIterator& operator=(const ReverseKeyIterator& other);
  ReverseKeyIterator(ReverseKeyIterator&&) noexcept;
  ReverseKeyIterator& operator=(ReverseKeyIterator&&) noexcept;

  auto operator*() const -> const Bytes&;
  auto operator++() -> ReverseKeyIterator&;
  void operator++(int);
  auto operator==(const ReverseKeyIterator&) const noexcept -> bool;
  auto operator==(std::default_sentinel_t) const noexcept -> bool;

private:
  explicit ReverseKeyIterator(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class DB;
  friend class Snapshot;
};

// ---------------------------------------------------------------------------
// ReverseEntryIterator — descending (key, value) walk.
//
// Move-only; uses default_sentinel_t for end detection.
// ---------------------------------------------------------------------------

class ReverseEntryIterator {
public:
  struct Impl;

  using iterator_concept = std::input_iterator_tag;
  using value_type       = EntryView;
  using difference_type  = std::ptrdiff_t;

  ReverseEntryIterator() noexcept;
  ~ReverseEntryIterator();
  ReverseEntryIterator(const ReverseEntryIterator&) = delete;
  ReverseEntryIterator& operator=(const ReverseEntryIterator&) = delete;
  ReverseEntryIterator(ReverseEntryIterator&&) noexcept;
  ReverseEntryIterator& operator=(ReverseEntryIterator&&) noexcept;

  auto operator*() const -> const EntryView&;
  auto operator++() -> ReverseEntryIterator&;
  void operator++(int);
  auto operator==(std::default_sentinel_t) const noexcept -> bool;

private:
  explicit ReverseEntryIterator(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class DB;
  friend class Snapshot;
};

// ---------------------------------------------------------------------------
// ChangeIterator — raw committed entries in ascending sequence order.
// Used for replication via DB::changes_since().
// ---------------------------------------------------------------------------

class ChangeIterator {
public:
  struct Impl;

  using iterator_concept = std::input_iterator_tag;
  using value_type       = DataEntryView;
  using difference_type  = std::ptrdiff_t;

  ChangeIterator() noexcept;
  ~ChangeIterator();
  ChangeIterator(const ChangeIterator&) = delete;
  ChangeIterator& operator=(const ChangeIterator&) = delete;
  ChangeIterator(ChangeIterator&&) noexcept;
  ChangeIterator& operator=(ChangeIterator&&) noexcept;

  auto operator*() const -> const DataEntryView&;
  auto operator++() -> ChangeIterator&;
  void operator++(int);
  auto operator==(std::default_sentinel_t) const noexcept -> bool;

private:
  explicit ChangeIterator(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class DB;
};

// ---------------------------------------------------------------------------
// Snapshot — frozen, move-only, read-only view of DB state.
// ---------------------------------------------------------------------------

class Snapshot {
public:
  struct Impl;

  ~Snapshot();
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot(Snapshot&&) noexcept;
  Snapshot& operator=(Snapshot&&) noexcept;

  [[nodiscard]] auto get(const ReadOptions& opts,
                         BytesView key,
                         Bytes& out) const -> bool;
  [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                  BytesView key) const -> bool;

  [[nodiscard]] auto iter_from(const ReadOptions& opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
  [[nodiscard]] auto keys_from(const ReadOptions& opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;
  [[nodiscard]] auto riter_from(const ReadOptions& opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>;
  [[nodiscard]] auto rkeys_from(const ReadOptions& opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

private:
  explicit Snapshot(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class DB;
  friend class WritePlan;
};

// ---------------------------------------------------------------------------
// FileManifest — sealed file inventory returned by DB::create_manifest().
// ---------------------------------------------------------------------------

struct FileManifest {
  Snapshot snap;
  std::vector<FileInfo> files;
  std::uint64_t through_sequence{0};
};

// ---------------------------------------------------------------------------
// WritePlan — conditional write + guard vocabulary for DB::apply_batch().
// ---------------------------------------------------------------------------

class WritePlan {
public:
  struct Impl;

  WritePlan();
  explicit WritePlan(Snapshot snap);
  ~WritePlan();
  WritePlan(const WritePlan&) = delete;
  WritePlan& operator=(const WritePlan&) = delete;
  WritePlan(WritePlan&&) noexcept;
  WritePlan& operator=(WritePlan&&) noexcept;

  void put(BytesView key, BytesView value);
  void del(BytesView key);
  void del_range(BytesView from, BytesView to);

  void ensure_present(BytesView key);
  void ensure_absent(BytesView key);
  // Requires snapshot; throws std::logic_error if called without one.
  void ensure_unchanged(BytesView key);
  void ensure_range_unchanged(BytesView from, BytesView to);

  [[nodiscard]] auto has_snapshot() const noexcept -> bool;

private:
  std::unique_ptr<Impl> impl_;
  friend class DB;
};

// ---------------------------------------------------------------------------
// DB — SWMR key-value store. Non-copyable and non-moveable.
// ---------------------------------------------------------------------------

class DB {
public:
  struct Impl;

  [[nodiscard]] static auto open(std::filesystem::path dir,
                                 Options opts = {}) -> DB;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;
  DB(DB&&) = delete;
  DB& operator=(DB&&) = delete;
  ~DB();

  [[nodiscard]] auto get(const ReadOptions& opts,
                         BytesView key,
                         Bytes& out) const -> bool;
  void put(const WriteOptions& opts,
           BytesView key, BytesView value);
  [[nodiscard]] auto del(const WriteOptions& opts,
                         BytesView key) -> bool;
  void del_range(const WriteOptions& opts,
                 BytesView from, BytesView to);
  [[nodiscard]] auto contains_key(const ReadOptions& opts,
                                  BytesView key) const -> bool;

  [[nodiscard]] auto mode() const noexcept -> Mode;
  void set_mode(Mode mode);

  [[nodiscard]] auto is_degraded() const noexcept -> bool;
  [[nodiscard]] auto degraded_reason() const noexcept -> std::string;
  void resume();

  [[nodiscard]] auto snapshot() const -> Snapshot;
  [[nodiscard]] auto apply_batch(WriteOptions opts,
                                 WritePlan plan) -> bool;

  [[nodiscard]] auto iter_from(const ReadOptions& opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<EntryIterator, std::default_sentinel_t>;
  [[nodiscard]] auto keys_from(const ReadOptions& opts,
                               BytesView from = {}) const
      -> std::ranges::subrange<KeyIterator, std::default_sentinel_t>;
  [[nodiscard]] auto riter_from(const ReadOptions& opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseEntryIterator, std::default_sentinel_t>;
  [[nodiscard]] auto rkeys_from(const ReadOptions& opts,
                                BytesView from = {}) const
      -> std::ranges::subrange<ReverseKeyIterator, ReverseKeyIterator>;

  [[nodiscard]] auto vacuum(VacuumOptions opts = {}) -> bool;

  [[nodiscard]] auto current_sequence(
      std::chrono::milliseconds timeout = std::chrono::milliseconds{0}) const
      -> std::uint64_t;

  [[nodiscard]] auto create_manifest() -> FileManifest;

  [[nodiscard]] auto changes_since(const Snapshot& snap,
                                   std::uint64_t from_sequence) const
      -> std::ranges::subrange<ChangeIterator, std::default_sentinel_t>;

  void ingest(std::span<const DataEntryView> entries);

  [[nodiscard]] auto stats() const -> std::map<std::string, std::int64_t>;

private:
  explicit DB(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace bytecask::internal

// ---------------------------------------------------------------------------
// bytecask namespace aliases — bring all bytecask::internal types into bytecask.
//
// This allows callers to write bytecask::DB, bytecask::WriteOptions, etc.
// Guarded so the impl file (which imports the module) does not see these
// aliases, which would conflict with the module-exported types.
// ---------------------------------------------------------------------------

#ifndef BYTECASK_HPP_IMPL_MODE
namespace bytecask {

using Bytes                = internal::Bytes;
using BytesView            = internal::BytesView;
using Mode                 = internal::Mode;
using EntryType            = internal::EntryType;
using WriteOptions         = internal::WriteOptions;
using ReadOptions          = internal::ReadOptions;
using VacuumOptions        = internal::VacuumOptions;
using Options              = internal::Options;
using SizeLimits           = internal::SizeLimits;
using FileInfo             = internal::FileInfo;
using DataEntryView        = internal::DataEntryView;
using EntryView            = internal::EntryView;
using DbDegraded           = internal::DbDegraded;
using DbFollowerMode       = internal::DbFollowerMode;
using DB                   = internal::DB;
using Snapshot             = internal::Snapshot;
using WritePlan            = internal::WritePlan;
using FileManifest         = internal::FileManifest;
using KeyIterator          = internal::KeyIterator;
using EntryIterator             = internal::EntryIterator;
using ReverseKeyIterator        = internal::ReverseKeyIterator;
using ReverseEntryIterator      = internal::ReverseEntryIterator;
using ChangeIterator            = internal::ChangeIterator;

} // namespace bytecask
#endif // BYTECASK_HPP_IMPL_MODE
