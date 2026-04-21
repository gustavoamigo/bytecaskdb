// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_embind.cpp — Embind binding layer for ByteCaskDB WASM.
//
// Exposes DB, Snapshot, and WritePlan to JavaScript via Emscripten's Embind.
// All byte conversions copy data across the JS/WASM boundary — no dangling
// views into linear memory.

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
// JsDB bound methods
// ---------------------------------------------------------------------------

static auto jsdb_open(const std::string &path) -> JsDB * {
  return new JsDB{std::filesystem::path{path},
                  bytecask::Options{.recovery_threads = 1}};
}

static auto jsdb_get(JsDB &self, const std::string &key) -> val {
  Bytes out;
  if (self.db.get({}, to_view(key), out)) {
    return bytes_to_js(out);
  }
  return val::null();
}

static void jsdb_put(JsDB &self, const std::string &key,
                     const std::string &value) {
  self.db.put({.sync = false}, to_view(key), to_view(value));
}

static void jsdb_put_sync(JsDB &self, const std::string &key,
                          const std::string &value) {
  self.db.put({.sync = true}, to_view(key), to_view(value));
}

static auto jsdb_del(JsDB &self, const std::string &key) -> bool {
  return self.db.del({.sync = false}, to_view(key));
}

static auto jsdb_del_sync(JsDB &self, const std::string &key) -> bool {
  return self.db.del({.sync = true}, to_view(key));
}

static void jsdb_del_range(JsDB &self, const std::string &from,
                           const std::string &to) {
  self.db.del_range({.sync = false}, to_view(from), to_view(to));
}

static auto jsdb_contains_key(JsDB &self, const std::string &key) -> bool {
  return self.db.contains_key(to_view(key));
}

static auto jsdb_snapshot(JsDB &self) -> JsSnapshot * {
  return new JsSnapshot{self.db.snapshot()};
}

static auto jsdb_apply_batch(JsDB &self, JsWritePlan &plan) -> bool {
  plan.check();
  auto result = self.db.apply_batch({.sync = true}, std::move(*plan.plan));
  plan.plan.reset();
  return result;
}

static auto jsdb_apply_batch_nosync(JsDB &self, JsWritePlan &plan) -> bool {
  plan.check();
  auto result = self.db.apply_batch({.sync = false}, std::move(*plan.plan));
  plan.plan.reset();
  return result;
}

static auto jsdb_entries(JsDB &self, const std::string &from, int limit)
    -> val {
  auto result = val::array();
  int count = 0;
  for (auto &[key, value] : self.db.iter_from({}, to_view(from))) {
    if (count >= limit) break;
    auto entry = val::object();
    entry.set("key", key_to_js(key));
    entry.set("value", bytes_to_js(value));
    result.call<void>("push", entry);
    ++count;
  }
  return result;
}

static auto jsdb_keys(JsDB &self, const std::string &from, int limit) -> val {
  auto result = val::array();
  int count = 0;
  for (auto &key : self.db.keys_from({}, to_view(from))) {
    if (count >= limit) break;
    result.call<void>("push", key_to_js(key));
    ++count;
  }
  return result;
}

static auto jsdb_entries_reverse(JsDB &self, const std::string &from,
                                 int limit) -> val {
  auto result = val::array();
  int count = 0;
  for (auto &[key, value] : self.db.riter_from({}, to_view(from))) {
    if (count >= limit) break;
    auto entry = val::object();
    entry.set("key", key_to_js(key));
    entry.set("value", bytes_to_js(value));
    result.call<void>("push", entry);
    ++count;
  }
  return result;
}

static auto jsdb_keys_reverse(JsDB &self, const std::string &from, int limit)
    -> val {
  auto result = val::array();
  int count = 0;
  for (auto &key : self.db.rkeys_from({}, to_view(from))) {
    if (count >= limit) break;
    result.call<void>("push", key_to_js(key));
    ++count;
  }
  return result;
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

static auto jssnap_get(JsSnapshot &self, const std::string &key) -> val {
  self.check();
  Bytes out;
  if (self.snap->get(to_view(key), out)) {
    return bytes_to_js(out);
  }
  return val::null();
}

static auto jssnap_contains_key(JsSnapshot &self, const std::string &key)
    -> bool {
  self.check();
  return self.snap->contains_key(to_view(key));
}

static auto jssnap_entries(JsSnapshot &self, const std::string &from,
                           int limit) -> val {
  self.check();
  auto result = val::array();
  int count = 0;
  for (auto &[key, value] : self.snap->iter_from(to_view(from))) {
    if (count >= limit) break;
    auto entry = val::object();
    entry.set("key", key_to_js(key));
    entry.set("value", bytes_to_js(value));
    result.call<void>("push", entry);
    ++count;
  }
  return result;
}

static auto jssnap_keys(JsSnapshot &self, const std::string &from, int limit)
    -> val {
  self.check();
  auto result = val::array();
  int count = 0;
  for (auto &key : self.snap->keys_from(to_view(from))) {
    if (count >= limit) break;
    result.call<void>("push", key_to_js(key));
    ++count;
  }
  return result;
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
// Embind registration
// ---------------------------------------------------------------------------

EMSCRIPTEN_BINDINGS(bytecask) {
  class_<JsDB>("ByteCaskDB")
      .class_function("open", &jsdb_open, allow_raw_pointers())
      .function("get", &jsdb_get)
      .function("put", &jsdb_put)
      .function("putSync", &jsdb_put_sync)
      .function("del", &jsdb_del)
      .function("delSync", &jsdb_del_sync)
      .function("delRange", &jsdb_del_range)
      .function("containsKey", &jsdb_contains_key)
      .function("snapshot", &jsdb_snapshot, allow_raw_pointers())
      .function("applyBatch", &jsdb_apply_batch)
      .function("applyBatchNoSync", &jsdb_apply_batch_nosync)
      .function("entries", &jsdb_entries)
      .function("keys", &jsdb_keys)
      .function("entriesReverse", &jsdb_entries_reverse)
      .function("keysReverse", &jsdb_keys_reverse)
      .function("vacuum", &jsdb_vacuum)
      .function("isDegraded", &jsdb_is_degraded)
      .function("degradedReason", &jsdb_degraded_reason)
      .function("resume", &jsdb_resume)
      .function("close", &jsdb_close);

  class_<JsSnapshot>("Snapshot")
      .function("get", &jssnap_get)
      .function("containsKey", &jssnap_contains_key)
      .function("entries", &jssnap_entries)
      .function("keys", &jssnap_keys)
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
}
