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
#include <utility>
#include <vector>

#include <emscripten/bind.h>
#include <emscripten/val.h>

import bytecask;

using namespace emscripten;
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

// ---------------------------------------------------------------------------
// Options extraction helpers
// ---------------------------------------------------------------------------

static auto has_prop(const val &opts, const char *name) -> bool {
  return !opts.isUndefined() && !opts.isNull() && opts.hasOwnProperty(name);
}

static auto extract_write_options(const val &opts) -> bytecask::WriteOptions {
  bytecask::WriteOptions wo;
  if (has_prop(opts, "sync")) wo.sync = opts["sync"].as<bool>();
  return wo;
}

static auto extract_read_options(const val &opts) -> bytecask::ReadOptions {
  bytecask::ReadOptions ro;
  if (has_prop(opts, "verifyChecksums"))
    ro.verify_checksums = opts["verifyChecksums"].as<bool>();
  return ro;
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
    if (!snap) throw std::runtime_error("Snapshot consumed by WritePlan");
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
    if (!plan) throw std::runtime_error("WritePlan already applied");
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
    auto [key, value] = *it;
    auto entry = val::object();
    entry.set("key", key_to_js(key));
    entry.set("value", bytes_to_js(value));
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
                        bytecask::ReverseEntryIterator> range;
  bytecask::ReverseEntryIterator it;
  bytecask::ReverseEntryIterator end;

  explicit JsReverseEntryIterator(
      std::ranges::subrange<bytecask::ReverseEntryIterator,
                            bytecask::ReverseEntryIterator> r)
      : range{std::move(r)}, it{range.begin()}, end{range.end()} {}

  auto next() -> val {
    auto result = val::object();
    if (it == end) {
      result.set("done", true);
      return result;
    }
    auto [key, value] = *it;
    auto entry = val::object();
    entry.set("key", key_to_js(key));
    entry.set("value", bytes_to_js(value));
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

static auto mode_to_string(bytecask::Mode m) -> const char * {
  switch (m) {
    case bytecask::Mode::Leader: return "leader";
    case bytecask::Mode::Follower: return "follower";
  }
}

static auto span_to_js(std::span<const std::byte> s) -> val {
  return to_js_uint8array(s.data(), s.size());
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
    entry.set("sequence", static_cast<double>(view.sequence));
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
  double through_sequence;
};

// ---------------------------------------------------------------------------
// JsDB bound methods
// ---------------------------------------------------------------------------

static auto jsdb_open(const std::string &path, val opts) -> JsDB * {
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
  return new JsDB{std::filesystem::path{path}, std::move(o)};
}

static auto jsdb_get(JsDB &self, const std::string &key, val opts) -> val {
  auto ro = extract_read_options(opts);
  Bytes out;
  if (self.db.get(ro, to_view(key), out)) {
    return bytes_to_js(out);
  }
  return val::null();
}

static void jsdb_put(JsDB &self, const std::string &key,
                     const std::string &value, val opts) {
  auto wo = extract_write_options(opts);
  self.db.put(wo, to_view(key), to_view(value));
}

static auto jsdb_del(JsDB &self, const std::string &key, val opts) -> bool {
  auto wo = extract_write_options(opts);
  return self.db.del(wo, to_view(key));
}

static void jsdb_del_range(JsDB &self, const std::string &from,
                           const std::string &to, val opts) {
  auto wo = extract_write_options(opts);
  self.db.del_range(wo, to_view(from), to_view(to));
}

static auto jsdb_contains_key(JsDB &self, const std::string &key, val opts)
    -> bool {
  auto ro = extract_read_options(opts);
  return self.db.contains_key(ro, to_view(key));
}

static auto jsdb_snapshot(JsDB &self) -> JsSnapshot * {
  return new JsSnapshot{self.db.snapshot()};
}

static auto jsdb_apply_batch(JsDB &self, JsWritePlan &plan, val opts) -> bool {
  plan.check();
  auto wo = extract_write_options(opts);
  auto result = self.db.apply_batch(wo, std::move(*plan.plan));
  plan.plan.reset();
  return result;
}

static auto jsdb_entries(JsDB &self, const std::string &from, val opts)
    -> JsEntryIterator * {
  auto ro = extract_read_options(opts);
  return new JsEntryIterator{self.db.iter_from(ro, to_view(from))};
}

static auto jsdb_keys(JsDB &self, const std::string &from, val opts)
    -> JsKeyIterator * {
  auto ro = extract_read_options(opts);
  return new JsKeyIterator{self.db.keys_from(ro, to_view(from))};
}

static auto jsdb_entries_reverse(JsDB &self, const std::string &from, val opts)
    -> JsReverseEntryIterator * {
  auto ro = extract_read_options(opts);
  return new JsReverseEntryIterator{self.db.riter_from(ro, to_view(from))};
}

static auto jsdb_keys_reverse(JsDB &self, const std::string &from, val opts)
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

static auto jsdb_mode(JsDB &self) -> std::string {
  return mode_to_string(self.db.mode());
}

static void jsdb_set_mode(JsDB &self, const std::string &mode) {
  self.db.set_mode(string_to_mode(mode));
}

static auto jsdb_current_sequence(JsDB &self, val timeout_ms_val) -> double {
  std::uint64_t timeout_ms = 0;
  if (!timeout_ms_val.isUndefined() && !timeout_ms_val.isNull()) {
    timeout_ms = timeout_ms_val.as<std::uint64_t>();
  }
  return static_cast<double>(
      self.db.current_sequence(std::chrono::milliseconds{timeout_ms}));
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
      snap, std::move(js_files),
      static_cast<double>(manifest.through_sequence)};
}

static auto jsdb_changes_since(JsDB &self, JsSnapshot &snap,
                               double from_seq) -> JsChangeIterator * {
  snap.check();
  auto range = self.db.changes_since(*snap.snap,
                                     static_cast<std::uint64_t>(from_seq));
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
    auto seq = static_cast<std::uint64_t>(e["sequence"].as<double>());
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

static void jsdb_close(JsDB &self) { delete &self; }

// ---------------------------------------------------------------------------
// JsSnapshot bound methods
// ---------------------------------------------------------------------------

static auto jssnap_get(JsSnapshot &self, const std::string &key, val opts)
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
                                val opts) -> bool {
  self.check();
  auto ro = extract_read_options(opts);
  return self.snap->contains_key(ro, to_view(key));
}

static auto jssnap_entries(JsSnapshot &self, const std::string &from, val opts)
    -> JsEntryIterator * {
  self.check();
  auto ro = extract_read_options(opts);
  return new JsEntryIterator{self.snap->iter_from(ro, to_view(from))};
}

static auto jssnap_keys(JsSnapshot &self, const std::string &from, val opts)
    -> JsKeyIterator * {
  self.check();
  auto ro = extract_read_options(opts);
  return new JsKeyIterator{self.snap->keys_from(ro, to_view(from))};
}

static auto jssnap_entries_reverse(JsSnapshot &self, const std::string &from,
                                   val opts) -> JsReverseEntryIterator * {
  self.check();
  auto ro = extract_read_options(opts);
  return new JsReverseEntryIterator{self.snap->riter_from(ro, to_view(from))};
}

static auto jssnap_keys_reverse(JsSnapshot &self, const std::string &from,
                                val opts) -> JsReverseKeyIterator * {
  self.check();
  auto ro = extract_read_options(opts);
  return new JsReverseKeyIterator{self.snap->rkeys_from(ro, to_view(from))};
}

static void jssnap_close(JsSnapshot &self) { delete &self; }

// ---------------------------------------------------------------------------
// JsWritePlan bound methods
// ---------------------------------------------------------------------------

static auto jswp_with_snapshot(JsSnapshot &snap) -> JsWritePlan * {
  return new JsWritePlan{snap};
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

static void jswp_close(JsWritePlan &self) { delete &self; }

// ---------------------------------------------------------------------------
// Iterator close helpers
// ---------------------------------------------------------------------------

static void js_entry_iter_close(JsEntryIterator &self) { delete &self; }
static void js_key_iter_close(JsKeyIterator &self) { delete &self; }
static void js_rev_entry_iter_close(JsReverseEntryIterator &self) { delete &self; }
static void js_rev_key_iter_close(JsReverseKeyIterator &self) { delete &self; }
static void js_change_iter_close(JsChangeIterator &self) { delete &self; }
static void js_file_manifest_close(JsFileManifest &self) {
  // snapshot is owned separately, caller must close it.
  delete &self;
}

// ---------------------------------------------------------------------------
// Embind registration
// ---------------------------------------------------------------------------

EMSCRIPTEN_BINDINGS(bytecask) {
  class_<JsDB>("ByteCaskDB")
      .class_function("open", &jsdb_open, allow_raw_pointers())
      .function("get", &jsdb_get)
      .function("put", &jsdb_put)
      .function("del", &jsdb_del)
      .function("delRange", &jsdb_del_range)
      .function("containsKey", &jsdb_contains_key)
      .function("snapshot", &jsdb_snapshot, allow_raw_pointers())
      .function("applyBatch", &jsdb_apply_batch)
      .function("entries", &jsdb_entries, allow_raw_pointers())
      .function("keys", &jsdb_keys, allow_raw_pointers())
      .function("entriesReverse", &jsdb_entries_reverse, allow_raw_pointers())
      .function("keysReverse", &jsdb_keys_reverse, allow_raw_pointers())
      .function("vacuum", &jsdb_vacuum)
      .function("isDegraded", &jsdb_is_degraded)
      .function("degradedReason", &jsdb_degraded_reason)
      .function("resume", &jsdb_resume)
      .function("mode", &jsdb_mode)
      .function("setMode", &jsdb_set_mode)
      .function("currentSequence", &jsdb_current_sequence)
      .function("createManifest", &jsdb_create_manifest, allow_raw_pointers())
      .function("changesSince", &jsdb_changes_since, allow_raw_pointers())
      .function("ingest", &jsdb_ingest)
      .function("close", &jsdb_close);

  class_<JsSnapshot>("Snapshot")
      .function("get", &jssnap_get)
      .function("containsKey", &jssnap_contains_key)
      .function("entries", &jssnap_entries, allow_raw_pointers())
      .function("keys", &jssnap_keys, allow_raw_pointers())
      .function("entriesReverse", &jssnap_entries_reverse, allow_raw_pointers())
      .function("keysReverse", &jssnap_keys_reverse, allow_raw_pointers())
      .function("close", &jssnap_close);

  class_<JsWritePlan>("WritePlan")
      .constructor<>()
      .class_function("withSnapshot", &jswp_with_snapshot,
                      allow_raw_pointers())
      .function("put", &jswp_put)
      .function("del", &jswp_del)
      .function("delRange", &jswp_del_range)
      .function("ensurePresent", &jswp_ensure_present)
      .function("ensureAbsent", &jswp_ensure_absent)
      .function("ensureUnchanged", &jswp_ensure_unchanged)
      .function("ensureRangeUnchanged", &jswp_ensure_range_unchanged)
      .function("close", &jswp_close);

  class_<JsEntryIterator>("EntryIterator")
      .function("next", &JsEntryIterator::next)
      .function("close", &js_entry_iter_close);

  class_<JsKeyIterator>("KeyIterator")
      .function("next", &JsKeyIterator::next)
      .function("close", &js_key_iter_close);

  class_<JsReverseEntryIterator>("ReverseEntryIterator")
      .function("next", &JsReverseEntryIterator::next)
      .function("close", &js_rev_entry_iter_close);

  class_<JsReverseKeyIterator>("ReverseKeyIterator")
      .function("next", &JsReverseKeyIterator::next)
      .function("close", &js_rev_key_iter_close);

  class_<JsChangeIterator>("ChangeIterator")
      .function("next", &JsChangeIterator::next)
      .function("close", &js_change_iter_close);

  class_<JsFileManifest>("FileManifest")
      .function("getSnapshot", [](JsFileManifest &self) -> JsSnapshot * {
        return self.snapshot;
      }, allow_raw_pointers())
      .function("getFiles", [](JsFileManifest &self) -> val {
        return self.files;
      })
      .function("getThroughSequence", [](JsFileManifest &self) -> double {
        return self.through_sequence;
      })
      .function("close", &js_file_manifest_close);
}
