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
}
