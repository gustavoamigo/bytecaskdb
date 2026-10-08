// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_embind.cpp — Embind binding layer for ByteCaskDB WASM.
//
// Exposes DB, Snapshot, WritePlan, and lazy iterators to JavaScript via
// Emscripten's Embind. All byte conversions copy data across the JS/WASM
// boundary — no dangling views into linear memory.
//
// API design: every method that accepts options in the C++ API takes an
// optional JS options object as the last parameter. No behavioral options
// are baked into method names. See emscripten/API.md for the full spec.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

import bytecask;

#include "../shared/error_codes.h"
#ifdef BYTECASK_TESTING
#include "../../bytecaskdb/fault_injector.h"
#endif

using namespace emscripten;
using bytecask_node::HandleClosed;

// ---------------------------------------------------------------------------
// Error translation
// ---------------------------------------------------------------------------

// The JS Error a call throws for a C++ exception: its message, a `code`
// (shared/error_codes.h) and, for BC_IO, the `errno`. Under
// -fwasm-exceptions a C++ exception that reached JS would carry neither
// its type nor its what().
static auto make_js_error(const std::exception &e) -> val {
  const auto info = bytecask_node::classify_error(e);
  auto error = val::global("Error").new_(std::string{e.what()});
  error.set("code", std::string{info.code});
  if (info.errno_value != 0) error.set("errno", info.errno_value);
  return error;
}

// Guarded<&f>::call is what EMSCRIPTEN_BINDINGS registers in place of f: it
// runs f and turns an exception into make_js_error's Error. The JS throw
// happens after the catch block has ended, so the C++ exception is freed
// before JS unwinds the frame.
template <auto F> struct Guarded;

template <typename R, typename... A, R (*F)(A...)> struct Guarded<F> {
  static auto call(A... args) -> R {
    std::optional<val> error;
    try {
      return F(std::forward<A>(args)...);
    } catch (const std::exception &e) {
      error = make_js_error(e);
    }
    error->throw_();
  }
};

template <typename R, typename C, typename... A, R (C::*F)(A...)>
struct Guarded<F> {
  static auto call(C &self, A... args) -> R {
    std::optional<val> error;
    try {
      return (self.*F)(std::forward<A>(args)...);
    } catch (const std::exception &e) {
      error = make_js_error(e);
    }
    error->throw_();
  }
};
using bytecask::Bytes;
using bytecask::BytesView;

// ---------------------------------------------------------------------------
// Byte conversion helpers
// ---------------------------------------------------------------------------

static auto to_view(const std::string &s) -> BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
}

// Copy a C++ byte buffer into a JS-owned Uint8Array.
// Must copy — a typed_memory_view is invalidated when the C++ buffer destructs.
static auto to_js_uint8array(const std::byte *data, std::size_t size) -> val {
  auto js_arr = val::global("Uint8Array").new_(static_cast<int>(size));
  js_arr.call<void>("set", val(typed_memory_view(size,
      reinterpret_cast<const uint8_t *>(data))));
  return js_arr;
}

static auto bytes_to_js(const Bytes &b) -> val {
  return to_js_uint8array(b.data(), b.size());
}

static auto key_to_js(const bytecask::Key &k) -> val {
  return to_js_uint8array(&*k.begin(), k.size());
}

static auto span_to_js(std::span<const std::byte> s) -> val {
  return to_js_uint8array(s.data(), s.size());
}

// ---------------------------------------------------------------------------
// Options extraction helpers
// ---------------------------------------------------------------------------

// A trailing parameter JS callers may leave out. Embind accepts fewer
// arguments only when the missing ones are std::optional: a build with
// ASSERTIONS (node-nightly.yml's checked build) refuses the call otherwise,
// and a release build passes undefined only because it skips the check.
using OptionalVal = std::optional<val>;

static auto value_or_undefined(const OptionalVal &v) -> val {
  return v.value_or(val::undefined());
}

static auto has_prop(const val &opts, const char *name) -> bool {
  return !opts.isUndefined() && !opts.isNull() && opts.hasOwnProperty(name);
}

static auto extract_write_options(const OptionalVal &opts_arg)
    -> bytecask::WriteOptions {
  const auto opts = value_or_undefined(opts_arg);
  bytecask::WriteOptions wo;
  if (has_prop(opts, "sync")) wo.sync = opts["sync"].as<bool>();
  return wo;
}

static auto extract_read_options(const OptionalVal &opts_arg)
    -> bytecask::ReadOptions {
  const auto opts = value_or_undefined(opts_arg);
  bytecask::ReadOptions ro;
  if (has_prop(opts, "verifyChecksums"))
    ro.verify_checksums = opts["verifyChecksums"].as<bool>();
  return ro;
}

// Converts a CommitResult to a JS {sequence: bigint, durable: boolean} object.
// sequence is bound as a plain uint64_t — with -sWASM_BIGINT this crosses
// the JS boundary as an exact BigInt, never a lossy double.
static auto commit_result_to_js(const bytecask::CommitResult &r) -> val {
  auto obj = val::object();
  obj.set("sequence", r.sequence);
  obj.set("durable", r.durable);
  return obj;
}

// ---------------------------------------------------------------------------
// JsDB — non-copyable, non-moveable DB wrapper
// ---------------------------------------------------------------------------

struct JsDB {
  bytecask::DB db;

  JsDB(std::filesystem::path dir, bytecask::Options opts)
      : db{bytecask::DB::open(std::move(dir), std::move(opts))} {}
};

// ---------------------------------------------------------------------------
// JsSnapshot — move-only snapshot with consumption guard
// ---------------------------------------------------------------------------

struct JsSnapshot {
  std::optional<bytecask::Snapshot> snap;

  explicit JsSnapshot(bytecask::Snapshot s) : snap{std::move(s)} {}

  void check() const {
    if (!snap) throw HandleClosed("Snapshot consumed by WritePlan");
  }
};

// ---------------------------------------------------------------------------
// JsWritePlan — move-only plan with consumption guard
// ---------------------------------------------------------------------------

struct JsWritePlan {
  std::optional<bytecask::WritePlan> plan;

  JsWritePlan() : plan{bytecask::WritePlan{}} {}

  explicit JsWritePlan(JsSnapshot &snap) {
    snap.check();
    plan.emplace(std::move(*snap.snap));
    snap.snap.reset();
  }

  void check() const {
    if (!plan) throw HandleClosed("WritePlan already applied");
  }
};

// ---------------------------------------------------------------------------
// Lazy iterator wrappers — implement JS iterator protocol via next()
// ---------------------------------------------------------------------------

struct JsEntryIterator {
  std::ranges::subrange<bytecask::EntryIterator, std::default_sentinel_t> range;
  bytecask::EntryIterator it;

  explicit JsEntryIterator(
      std::ranges::subrange<bytecask::EntryIterator, std::default_sentinel_t> r)
      : range{std::move(r)}, it{range.begin()} {}

  auto next() -> val {
    auto result = val::object();
    if (it == std::default_sentinel) {
      result.set("done", true);
      return result;
    }
    const auto &e = *it;
    auto entry = val::object();
    entry.set("key", span_to_js(e.key));
    entry.set("value", span_to_js(e.value));
    result.set("value", entry);
    result.set("done", false);
    ++it;
    return result;
  }
};

struct JsKeyIterator {
  std::ranges::subrange<bytecask::KeyIterator, std::default_sentinel_t> range;
  bytecask::KeyIterator it;

  explicit JsKeyIterator(
      std::ranges::subrange<bytecask::KeyIterator, std::default_sentinel_t> r)
      : range{std::move(r)}, it{range.begin()} {}

  auto next() -> val {
    auto result = val::object();
    if (it == std::default_sentinel) {
      result.set("done", true);
      return result;
    }
    auto key = *it;
    result.set("value", key_to_js(key));
    result.set("done", false);
    ++it;
    return result;
  }
};

struct JsReverseEntryIterator {
  std::ranges::subrange<bytecask::ReverseEntryIterator,
                        std::default_sentinel_t> range;
  bytecask::ReverseEntryIterator it;

  explicit JsReverseEntryIterator(
      std::ranges::subrange<bytecask::ReverseEntryIterator,
                            std::default_sentinel_t> r)
      : range{std::move(r)}, it{range.begin()} {}

  auto next() -> val {
    auto result = val::object();
    if (it == std::default_sentinel) {
      result.set("done", true);
      return result;
    }
    const auto &e = *it;
    auto entry = val::object();
    entry.set("key", span_to_js(e.key));
    entry.set("value", span_to_js(e.value));
    result.set("value", entry);
    result.set("done", false);
    ++it;
    return result;
  }
};

struct JsReverseKeyIterator {
  std::ranges::subrange<bytecask::ReverseKeyIterator,
                        bytecask::ReverseKeyIterator> range;
  bytecask::ReverseKeyIterator it;
  bytecask::ReverseKeyIterator end;

  explicit JsReverseKeyIterator(
      std::ranges::subrange<bytecask::ReverseKeyIterator,
                            bytecask::ReverseKeyIterator> r)
      : range{std::move(r)}, it{range.begin()}, end{range.end()} {}

  auto next() -> val {
    auto result = val::object();
    if (it == end) {
      result.set("done", true);
      return result;
    }
    auto key = *it;
    result.set("value", key_to_js(key));
    result.set("done", false);
    ++it;
    return result;
  }
};

// ---------------------------------------------------------------------------
// JsChangeIterator — wraps ChangeIterator for replication
// ---------------------------------------------------------------------------

static auto entry_type_to_string(bytecask::EntryType et) -> const char * {
  switch (et) {
    case bytecask::EntryType::Put: return "put";
    case bytecask::EntryType::Delete: return "delete";
    case bytecask::EntryType::BulkBegin: return "bulkBegin";
    case bytecask::EntryType::BulkEnd: return "bulkEnd";
    case bytecask::EntryType::RangeDel: return "rangeDel";
  }
}

static auto string_to_mode(const std::string &s) -> bytecask::Mode {
  if (s == "leader") return bytecask::Mode::Leader;
  if (s == "follower") return bytecask::Mode::Follower;
  throw std::invalid_argument("Invalid mode: " + s + " (expected 'leader' or 'follower')");
}

// 'mmap' parses here like any other backend, but DB::open rejects it on
// Emscripten builds (see bytecask.cppm): mmap emulation would only add a
// second copy of the data file into the WASM heap.
static auto string_to_io_backend(const std::string &s) -> bytecask::IoBackend {
  if (s == "pread") return bytecask::IoBackend::Pread;
  if (s == "mmap") return bytecask::IoBackend::Mmap;
  if (s == "bufferPool") return bytecask::IoBackend::BufferPool;
  throw std::invalid_argument(
      "Invalid ioBackend: " + s + " (expected 'pread', 'mmap', or 'bufferPool')");
}

static auto extract_buffer_pool_options(const val &opts) -> bytecask::BufferPoolOptions {
  bytecask::BufferPoolOptions bpo;
  if (has_prop(opts, "capacityBytes"))
    bpo.capacity_bytes = static_cast<std::size_t>(opts["capacityBytes"].as<uint64_t>());
  if (has_prop(opts, "directIo"))
    bpo.direct_io = opts["directIo"].as<bool>();
  return bpo;
}

static auto mode_to_string(bytecask::Mode m) -> const char * {
  switch (m) {
    case bytecask::Mode::Leader: return "leader";
    case bytecask::Mode::Follower: return "follower";
  }
}

struct JsChangeIterator {
  std::ranges::subrange<bytecask::ChangeIterator, std::default_sentinel_t> range;
  bytecask::ChangeIterator it;

  explicit JsChangeIterator(
      std::ranges::subrange<bytecask::ChangeIterator, std::default_sentinel_t> r)
      : range{std::move(r)}, it{range.begin()} {}

  auto next() -> val {
    auto result = val::object();
    if (it == std::default_sentinel) {
      result.set("done", true);
      return result;
    }
    auto view = *it;
    auto entry = val::object();
    entry.set("sequence", view.sequence);
    entry.set("entryType", std::string{entry_type_to_string(view.entry_type)});
    entry.set("key", span_to_js(view.key));
    entry.set("value", span_to_js(view.value));
    result.set("value", entry);
    result.set("done", false);
    ++it;
    return result;
  }
};

// ---------------------------------------------------------------------------
// JsFileManifest — wraps FileManifest from create_manifest()
// ---------------------------------------------------------------------------

struct JsFileManifest {
  JsSnapshot *snapshot;
  val files;
  std::uint64_t through_sequence;
};

// ---------------------------------------------------------------------------
// JsDB bound methods
// ---------------------------------------------------------------------------

static auto jsdb_open(const std::string &path, OptionalVal opts_arg) -> JsDB * {
  const auto opts = value_or_undefined(opts_arg);
  bytecask::Options o{.recovery_threads = 1};
  if (has_prop(opts, "maxFileBytes"))
    o.max_file_bytes = opts["maxFileBytes"].as<uint64_t>();
  if (has_prop(opts, "failOnCrcErrors"))
    o.fail_recovery_on_crc_errors = opts["failOnCrcErrors"].as<bool>();
  if (has_prop(opts, "maxKeyBytes"))
    o.max_key_bytes = opts["maxKeyBytes"].as<uint32_t>();
  if (has_prop(opts, "maxValueBytes"))
    o.max_value_bytes = opts["maxValueBytes"].as<uint32_t>();
  if (has_prop(opts, "initialMode"))
    o.initial_mode = string_to_mode(opts["initialMode"].as<std::string>());
  if (has_prop(opts, "ioBackend"))
    o.io_backend = string_to_io_backend(opts["ioBackend"].as<std::string>());
  if (has_prop(opts, "bufferPool"))
    o.buffer_pool = extract_buffer_pool_options(opts["bufferPool"]);
  return new JsDB{std::filesystem::path{path}, std::move(o)};
}

static auto jsdb_get(JsDB &self, const std::string &key, OptionalVal opts) -> val {
  auto ro = extract_read_options(opts);
  Bytes out;
  if (self.db.get(ro, to_view(key), out)) {
    return bytes_to_js(out);
  }
  return val::null();
}

static auto jsdb_put(JsDB &self, const std::string &key,
                     const std::string &value, OptionalVal opts) -> val {
  auto wo = extract_write_options(opts);
  return commit_result_to_js(self.db.put(wo, to_view(key), to_view(value)));
}

static auto jsdb_del(JsDB &self, const std::string &key, OptionalVal opts) -> val {
  auto wo = extract_write_options(opts);
  auto result = self.db.del(wo, to_view(key));
  if (!result) return val::null();
  return commit_result_to_js(*result);
}

static auto jsdb_del_range(JsDB &self, const std::string &from,
                           const std::string &to, OptionalVal opts) -> val {
  auto wo = extract_write_options(opts);
  return commit_result_to_js(self.db.del_range(wo, to_view(from), to_view(to)));
}

static auto jsdb_contains_key(JsDB &self, const std::string &key, OptionalVal opts)
    -> bool {
  auto ro = extract_read_options(opts);
  return self.db.contains_key(ro, to_view(key));
}

static auto jsdb_snapshot(JsDB &self) -> JsSnapshot * {
  return new JsSnapshot{self.db.snapshot()};
}

static auto jsdb_apply_batch(JsDB &self, JsWritePlan &plan, OptionalVal opts) -> val {
  plan.check();
  auto wo = extract_write_options(opts);
  auto result = self.db.apply_batch(wo, std::move(*plan.plan));
  plan.plan.reset();
  if (!result) return val::null();
  return commit_result_to_js(*result);
}

static auto jsdb_entries(JsDB &self, const std::string &from, OptionalVal opts)
    -> JsEntryIterator * {
  auto ro = extract_read_options(opts);
  return new JsEntryIterator{self.db.iter_from(ro, to_view(from))};
}

static auto jsdb_keys(JsDB &self, const std::string &from, OptionalVal opts)
    -> JsKeyIterator * {
  auto ro = extract_read_options(opts);
  return new JsKeyIterator{self.db.keys_from(ro, to_view(from))};
}

static auto jsdb_entries_reverse(JsDB &self, const std::string &from, OptionalVal opts)
    -> JsReverseEntryIterator * {
  auto ro = extract_read_options(opts);
  return new JsReverseEntryIterator{self.db.riter_from(ro, to_view(from))};
}

static auto jsdb_keys_reverse(JsDB &self, const std::string &from, OptionalVal opts)
    -> JsReverseKeyIterator * {
  auto ro = extract_read_options(opts);
  return new JsReverseKeyIterator{self.db.rkeys_from(ro, to_view(from))};
}

static auto jsdb_vacuum(JsDB &self) -> bool { return self.db.vacuum(); }

static auto jsdb_is_degraded(JsDB &self) -> bool {
  return self.db.is_degraded();
}

static auto jsdb_degraded_reason(JsDB &self) -> std::string {
  return std::string{self.db.degraded_reason()};
}

static void jsdb_resume(JsDB &self) { self.db.resume(); }

// Bound as closeDb: the JS close() runs it, then deletes the handle whether
// or not it threw (dispose.ts).
static void jsdb_close(JsDB &self) { self.db.close(); }

static auto jsdb_mode(JsDB &self) -> std::string {
  return mode_to_string(self.db.mode());
}

static void jsdb_set_mode(JsDB &self, const std::string &mode) {
  self.db.set_mode(string_to_mode(mode));
}

// The single sequence primitive. minSequence/timeoutMs default to 0 (an
// immediate, non-blocking poll) when omitted from JS — Embind has no
// built-in default-argument support, so undefined/null is treated as 0.
// Returns a bigint (uint64_t crosses the JS boundary as BigInt under
// -sWASM_BIGINT, never a lossy double).
static auto jsdb_durable_sequence(JsDB &self, OptionalVal min_sequence_arg,
                                  OptionalVal timeout_ms_arg) -> std::uint64_t {
  const auto min_sequence_val = value_or_undefined(min_sequence_arg);
  const auto timeout_ms_val = value_or_undefined(timeout_ms_arg);
  std::uint64_t min_sequence = 0;
  if (!min_sequence_val.isUndefined() && !min_sequence_val.isNull()) {
    min_sequence = min_sequence_val.as<std::uint64_t>();
  }
  std::uint64_t timeout_ms = 0;
  if (!timeout_ms_val.isUndefined() && !timeout_ms_val.isNull()) {
    timeout_ms = timeout_ms_val.as<std::uint64_t>();
  }
  return self.db.durable_sequence(min_sequence,
                                  std::chrono::milliseconds{timeout_ms});
}

static auto jsdb_create_manifest(JsDB &self) -> JsFileManifest * {
  auto manifest = self.db.create_manifest();
  auto *snap = new JsSnapshot{std::move(manifest.snap)};

  auto js_files = val::array();
  for (std::size_t i = 0; i < manifest.files.size(); ++i) {
    auto &fi = manifest.files[i];
    auto obj = val::object();
    obj.set("fileId", fi.file_id);
    obj.set("dataPath", fi.data_path.string());
    obj.set("hintPath", fi.hint_path.string());
    js_files.call<void>("push", obj);
  }

  return new JsFileManifest{
      snap, std::move(js_files), manifest.through_sequence};
}

static auto jsdb_changes_since(JsDB &self, JsSnapshot &snap,
                               std::uint64_t from_seq) -> JsChangeIterator * {
  snap.check();
  auto range = self.db.changes_since(*snap.snap, from_seq);
  return new JsChangeIterator{std::move(range)};
}

static void jsdb_ingest(JsDB &self, val entries) {
  auto len = entries["length"].as<std::size_t>();
  std::vector<bytecask::DataEntryView> views;
  views.reserve(len);

  // Keep owned string buffers alive for the duration of ingest.
  std::vector<std::string> key_bufs;
  std::vector<std::string> val_bufs;
  key_bufs.reserve(len);
  val_bufs.reserve(len);

  for (std::size_t i = 0; i < len; ++i) {
    auto e = entries[i];
    auto seq = e["sequence"].as<std::uint64_t>();
    auto et_str = e["entryType"].as<std::string>();

    bytecask::EntryType et;
    if (et_str == "put") et = bytecask::EntryType::Put;
    else if (et_str == "delete") et = bytecask::EntryType::Delete;
    else if (et_str == "bulkBegin") et = bytecask::EntryType::BulkBegin;
    else if (et_str == "bulkEnd") et = bytecask::EntryType::BulkEnd;
    else if (et_str == "rangeDel") et = bytecask::EntryType::RangeDel;
    else throw std::invalid_argument("Invalid entryType: " + et_str);

    key_bufs.push_back(e["key"].as<std::string>());
    val_bufs.push_back(e["value"].as<std::string>());

    views.push_back(bytecask::DataEntryView{
        .sequence = seq,
        .entry_type = et,
        .key = to_view(key_bufs.back()),
        .value = to_view(val_bufs.back()),
    });
  }
  self.db.ingest(views);
}

// ---------------------------------------------------------------------------
// JsSnapshot bound methods
// ---------------------------------------------------------------------------

static auto jssnap_get(JsSnapshot &self, const std::string &key, OptionalVal opts)
    -> val {
  self.check();
  auto ro = extract_read_options(opts);
  Bytes out;
  if (self.snap->get(ro, to_view(key), out)) {
    return bytes_to_js(out);
  }
  return val::null();
}

static auto jssnap_contains_key(JsSnapshot &self, const std::string &key,
                                OptionalVal opts) -> bool {
  self.check();
  auto ro = extract_read_options(opts);
  return self.snap->contains_key(ro, to_view(key));
}

static auto jssnap_entries(JsSnapshot &self, const std::string &from, OptionalVal opts)
    -> JsEntryIterator * {
  self.check();
  auto ro = extract_read_options(opts);
  return new JsEntryIterator{self.snap->iter_from(ro, to_view(from))};
}

static auto jssnap_keys(JsSnapshot &self, const std::string &from, OptionalVal opts)
    -> JsKeyIterator * {
  self.check();
  auto ro = extract_read_options(opts);
  return new JsKeyIterator{self.snap->keys_from(ro, to_view(from))};
}

static auto jssnap_entries_reverse(JsSnapshot &self, const std::string &from,
                                   OptionalVal opts) -> JsReverseEntryIterator * {
  self.check();
  auto ro = extract_read_options(opts);
  return new JsReverseEntryIterator{self.snap->riter_from(ro, to_view(from))};
}

static auto jssnap_keys_reverse(JsSnapshot &self, const std::string &from,
                                OptionalVal opts) -> JsReverseKeyIterator * {
  self.check();
  auto ro = extract_read_options(opts);
  return new JsReverseKeyIterator{self.snap->rkeys_from(ro, to_view(from))};
}

// ---------------------------------------------------------------------------
// JsWritePlan bound methods
// ---------------------------------------------------------------------------

static auto jswp_with_snapshot(JsSnapshot &snap) -> JsWritePlan * {
  return new JsWritePlan{snap};
}

static auto jswp_with_limits(OptionalVal opts_arg) -> JsWritePlan * {
  const auto opts = value_or_undefined(opts_arg);
  bytecask::SizeLimits limits;
  if (has_prop(opts, "maxKeyBytes"))
    limits.max_key_bytes = opts["maxKeyBytes"].as<uint32_t>();
  if (has_prop(opts, "maxValueBytes"))
    limits.max_value_bytes = opts["maxValueBytes"].as<uint32_t>();
  auto *wp = new JsWritePlan();
  wp->plan.emplace(limits);
  return wp;
}

static void jswp_put(JsWritePlan &self, const std::string &key,
                     const std::string &value) {
  self.check();
  self.plan->put(to_view(key), to_view(value));
}

static void jswp_del(JsWritePlan &self, const std::string &key) {
  self.check();
  self.plan->del(to_view(key));
}

static void jswp_del_range(JsWritePlan &self, const std::string &from,
                           const std::string &to) {
  self.check();
  self.plan->del_range(to_view(from), to_view(to));
}

static void jswp_ensure_present(JsWritePlan &self, const std::string &key) {
  self.check();
  self.plan->ensure_present(to_view(key));
}

static void jswp_ensure_absent(JsWritePlan &self, const std::string &key) {
  self.check();
  self.plan->ensure_absent(to_view(key));
}

static void jswp_ensure_unchanged(JsWritePlan &self, const std::string &key) {
  self.check();
  self.plan->ensure_unchanged(to_view(key));
}

static void jswp_ensure_range_unchanged(JsWritePlan &self,
                                        const std::string &from,
                                        const std::string &to) {
  self.check();
  self.plan->ensure_range_unchanged(to_view(from), to_view(to));
}

static auto jswp_has_snapshot(JsWritePlan &self) -> bool {
  self.check();
  return self.plan->has_snapshot();
}

static auto js_file_manifest_get_snapshot(JsFileManifest &self) -> JsSnapshot * {
  return self.snapshot;
}
static auto js_file_manifest_get_files(JsFileManifest &self) -> val {
  return self.files;
}
static auto js_file_manifest_get_through_sequence(JsFileManifest &self) -> std::uint64_t {
  return self.through_sequence;
}

static auto jsdb_stats(JsDB &self) -> val {
  auto stats = self.db.stats();
  auto result = val::object();
  for (auto &[key, value] : stats) {
    result.set(key, static_cast<double>(value));
  }
  return result;
}

// ---------------------------------------------------------------------------
// Embind registration
// ---------------------------------------------------------------------------

#ifdef BYTECASK_TESTING
// Test builds only (wasm_embind_testing, never published): fail I/O at a
// named engine checkpoint, as the engine's own tests do, so a test reaches
// the degraded state through the public API. The WASM build has one thread.
static auto testing_injector() -> bytecask::testing::FaultInjector & {
  static auto *injector = new bytecask::testing::FaultInjector{};
  return *injector;
}

static void testing_fail_at(const std::string &name) {
  auto &injector = testing_injector();
  injector = bytecask::testing::FaultInjector{};
  injector.fail_at_name = name;
  bytecask::testing::active_injector = &injector;
}

static void testing_clear_fault() {
  bytecask::testing::active_injector = nullptr;
}
#endif

EMSCRIPTEN_BINDINGS(bytecask) {
  register_optional<val>();
#ifdef BYTECASK_TESTING
  function("testingFailAt", &testing_fail_at);
  function("testingClearFault", &testing_clear_fault);
#endif
  class_<JsDB>("ByteCaskDB")
      .class_function("open", &Guarded<&jsdb_open>::call, allow_raw_pointers())
      .function("get", &Guarded<&jsdb_get>::call)
      .function("put", &Guarded<&jsdb_put>::call)
      .function("del", &Guarded<&jsdb_del>::call)
      .function("delRange", &Guarded<&jsdb_del_range>::call)
      .function("containsKey", &Guarded<&jsdb_contains_key>::call)
      .function("snapshot", &Guarded<&jsdb_snapshot>::call, allow_raw_pointers())
      .function("applyBatch", &Guarded<&jsdb_apply_batch>::call)
      .function("entries", &Guarded<&jsdb_entries>::call, allow_raw_pointers())
      .function("keys", &Guarded<&jsdb_keys>::call, allow_raw_pointers())
      .function("entriesReverse", &Guarded<&jsdb_entries_reverse>::call, allow_raw_pointers())
      .function("keysReverse", &Guarded<&jsdb_keys_reverse>::call, allow_raw_pointers())
      .function("vacuum", &Guarded<&jsdb_vacuum>::call)
      .function("isDegraded", &Guarded<&jsdb_is_degraded>::call)
      .function("degradedReason", &Guarded<&jsdb_degraded_reason>::call)
      .function("resume", &Guarded<&jsdb_resume>::call)
      .function("closeDb", &Guarded<&jsdb_close>::call)
      .function("mode", &Guarded<&jsdb_mode>::call)
      .function("setMode", &Guarded<&jsdb_set_mode>::call)
      .function("durableSequence", &Guarded<&jsdb_durable_sequence>::call)
      .function("createManifest", &Guarded<&jsdb_create_manifest>::call, allow_raw_pointers())
      .function("changesSince", &Guarded<&jsdb_changes_since>::call, allow_raw_pointers())
      .function("ingest", &Guarded<&jsdb_ingest>::call)
      .function("stats", &Guarded<&jsdb_stats>::call);

  class_<JsSnapshot>("Snapshot")
      .function("get", &Guarded<&jssnap_get>::call)
      .function("containsKey", &Guarded<&jssnap_contains_key>::call)
      .function("entries", &Guarded<&jssnap_entries>::call, allow_raw_pointers())
      .function("keys", &Guarded<&jssnap_keys>::call, allow_raw_pointers())
      .function("entriesReverse", &Guarded<&jssnap_entries_reverse>::call, allow_raw_pointers())
      .function("keysReverse", &Guarded<&jssnap_keys_reverse>::call, allow_raw_pointers());

  class_<JsWritePlan>("WritePlan")
      .constructor<>()
      .class_function("withSnapshot", &Guarded<&jswp_with_snapshot>::call,
                      allow_raw_pointers())
      .class_function("withLimits", &Guarded<&jswp_with_limits>::call, allow_raw_pointers())
      .function("put", &Guarded<&jswp_put>::call)
      .function("del", &Guarded<&jswp_del>::call)
      .function("delRange", &Guarded<&jswp_del_range>::call)
      .function("ensurePresent", &Guarded<&jswp_ensure_present>::call)
      .function("ensureAbsent", &Guarded<&jswp_ensure_absent>::call)
      .function("ensureUnchanged", &Guarded<&jswp_ensure_unchanged>::call)
      .function("ensureRangeUnchanged", &Guarded<&jswp_ensure_range_unchanged>::call)
      .function("hasSnapshot", &Guarded<&jswp_has_snapshot>::call);

  class_<JsEntryIterator>("EntryIterator")
      .function("next", &Guarded<&JsEntryIterator::next>::call);

  class_<JsKeyIterator>("KeyIterator")
      .function("next", &Guarded<&JsKeyIterator::next>::call);

  class_<JsReverseEntryIterator>("ReverseEntryIterator")
      .function("next", &Guarded<&JsReverseEntryIterator::next>::call);

  class_<JsReverseKeyIterator>("ReverseKeyIterator")
      .function("next", &Guarded<&JsReverseKeyIterator::next>::call);

  class_<JsChangeIterator>("ChangeIterator")
      .function("next", &Guarded<&JsChangeIterator::next>::call);

  class_<JsFileManifest>("FileManifest")
      .function("getSnapshot", &Guarded<&js_file_manifest_get_snapshot>::call, allow_raw_pointers())
      .function("getFiles", &Guarded<&js_file_manifest_get_files>::call)
      .function("getThroughSequence", &Guarded<&js_file_manifest_get_through_sequence>::call);
}
