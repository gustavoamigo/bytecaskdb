// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_napi.cpp — node-addon-api (N-API) binding layer for ByteCaskDB.
//
// Native counterpart to bytecaskdb-node/wasm/bytecask_embind.cpp: exposes DB,
// Snapshot, WritePlan, and lazy iterators to JavaScript, but links the real
// multi-threaded engine directly through the public PIMPL header
// (include/bytecask.hpp) instead of cross-compiling to WASM. See
// docs/native_node_binding_design.md for the full design.
//
// All byte conversions copy data across the addon boundary — a std::span
// into engine-owned memory is never handed to JS as a view; it would dangle
// the moment the C++ buffer is destroyed.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <napi.h>

#include "../../include/bytecask.hpp"

using bytecask::Bytes;
using bytecask::BytesView;

// ---------------------------------------------------------------------------
// Byte conversion helpers
// ---------------------------------------------------------------------------

namespace {

auto to_view(const std::string& s) -> BytesView {
  return std::as_bytes(std::span{s.data(), s.size()});
}

// Extracts an owned std::string from a JS string, Buffer, or Uint8Array
// argument. Keys and values cross as UTF-8 strings today (binary keys are a
// documented non-goal — see docs/native_node_binding_design.md). Buffer is
// itself a Uint8Array subclass, so IsTypedArray() covers both; get()/entries
// etc. return plain Uint8Array (see span_to_buffer), and values round-tripped
// straight back into put()/etc. must be accepted here too.
auto arg_to_string(const Napi::Value& v) -> std::string {
  if (v.IsTypedArray()) {
    auto arr = v.As<Napi::Uint8Array>();
    return std::string{reinterpret_cast<const char*>(arr.Data()), arr.ByteLength()};
  }
  return v.As<Napi::String>().Utf8Value();
}

// Copies a C++ byte span into a JS-owned Uint8Array — matches the WASM/Embind
// backend's return type (see src/types.ts) so callers see the same type
// regardless of backend. Must copy — a zero-copy view would dangle once the
// source buffer (engine-owned, or a per-call local) is destroyed.
auto span_to_buffer(Napi::Env env, std::span<const std::byte> s) -> Napi::Value {
  auto array_buffer = Napi::ArrayBuffer::New(env, s.size());
  if (!s.empty()) {
    std::memcpy(array_buffer.Data(), s.data(), s.size());
  }
  return Napi::Uint8Array::New(env, s.size(), array_buffer, 0);
}

auto bytes_to_buffer(Napi::Env env, const Bytes& b) -> Napi::Value {
  return span_to_buffer(env, std::span<const std::byte>{b});
}

// ---------------------------------------------------------------------------
// Options extraction helpers
// ---------------------------------------------------------------------------

auto has_prop(const Napi::Value& opts, const char* name) -> bool {
  if (!opts.IsObject()) return false;
  auto obj = opts.As<Napi::Object>();
  return obj.HasOwnProperty(name);
}

auto opt_prop(const Napi::Value& opts, const char* name) -> Napi::Value {
  return opts.As<Napi::Object>().Get(name);
}

auto extract_write_options(const Napi::Value& opts) -> bytecask::WriteOptions {
  bytecask::WriteOptions wo;
  if (has_prop(opts, "sync")) wo.sync = opt_prop(opts, "sync").As<Napi::Boolean>();
  return wo;
}

auto extract_read_options(const Napi::Value& opts) -> bytecask::ReadOptions {
  bytecask::ReadOptions ro;
  if (has_prop(opts, "verifyChecksums"))
    ro.verify_checksums = opt_prop(opts, "verifyChecksums").As<Napi::Boolean>();
  return ro;
}

// Converts a CommitResult to a JS {sequence: bigint, durable: boolean}
// object. sequence crosses as an exact Napi::BigInt — never a lossy double —
// since JS numbers cannot represent every uint64_t value.
auto commit_result_to_js(Napi::Env env, const bytecask::CommitResult& r)
    -> Napi::Value {
  auto obj = Napi::Object::New(env);
  obj.Set("sequence", Napi::BigInt::New(env, r.sequence));
  obj.Set("durable", Napi::Boolean::New(env, r.durable));
  return obj;
}

auto string_to_mode(const std::string& s) -> bytecask::Mode {
  if (s == "leader") return bytecask::Mode::Leader;
  if (s == "follower") return bytecask::Mode::Follower;
  throw std::invalid_argument("Invalid mode: " + s + " (expected 'leader' or 'follower')");
}

auto mode_to_string(bytecask::Mode m) -> const char* {
  switch (m) {
    case bytecask::Mode::Leader: return "leader";
    case bytecask::Mode::Follower: return "follower";
  }
  throw std::logic_error("unreachable: unknown Mode");
}

auto entry_type_to_string(bytecask::EntryType et) -> const char* {
  switch (et) {
    case bytecask::EntryType::Put: return "put";
    case bytecask::EntryType::Delete: return "delete";
    case bytecask::EntryType::BulkBegin: return "bulkBegin";
    case bytecask::EntryType::BulkEnd: return "bulkEnd";
    case bytecask::EntryType::RangeDel: return "rangeDel";
  }
  throw std::logic_error("unreachable: unknown EntryType");
}

auto string_to_entry_type(const std::string& s) -> bytecask::EntryType {
  if (s == "put") return bytecask::EntryType::Put;
  if (s == "delete") return bytecask::EntryType::Delete;
  if (s == "bulkBegin") return bytecask::EntryType::BulkBegin;
  if (s == "bulkEnd") return bytecask::EntryType::BulkEnd;
  if (s == "rangeDel") return bytecask::EntryType::RangeDel;
  throw std::invalid_argument("Invalid entryType: " + s);
}

// A missing/undefined/null argument at `index` yields Env().Undefined(),
// which every extract_*_options helper above treats as "no options".
auto opt_arg(const Napi::CallbackInfo& info, std::size_t index) -> Napi::Value {
  if (index >= info.Length()) return info.Env().Undefined();
  auto v = info[index];
  if (v.IsNull()) return info.Env().Undefined();
  return v;
}

// A bigint argument that is missing/undefined/null defaults to 0 — the
// engine's "immediate, non-blocking poll" default for durable_sequence().
auto opt_bigint_arg(const Napi::CallbackInfo& info, std::size_t index)
    -> std::uint64_t {
  if (index >= info.Length()) return 0;
  auto v = info[index];
  if (v.IsUndefined() || v.IsNull()) return 0;
  bool lossless = false;
  return v.As<Napi::BigInt>().Uint64Value(&lossless);
}

auto opt_uint32_arg(const Napi::CallbackInfo& info, std::size_t index)
    -> std::uint32_t {
  if (index >= info.Length()) return 0;
  auto v = info[index];
  if (v.IsUndefined() || v.IsNull()) return 0;
  return v.As<Napi::Number>().Uint32Value();
}

}  // namespace

// ---------------------------------------------------------------------------
// Forward declarations — the wrapper classes reference each other's
// constructors (WritePlan from Snapshot, DB methods returning Snapshot, …).
// ---------------------------------------------------------------------------

class NapiSnapshot;
class NapiWritePlan;
class NapiEntryIterator;
class NapiKeyIterator;
class NapiReverseEntryIterator;
class NapiReverseKeyIterator;
class NapiChangeIterator;
class NapiFileManifest;

// ---------------------------------------------------------------------------
// NapiDB — wraps bytecask::DB (non-copyable, non-moveable)
// ---------------------------------------------------------------------------

class NapiDB : public Napi::ObjectWrap<NapiDB> {
 public:
  static auto Init(Napi::Env env, Napi::Object exports) -> void;
  explicit NapiDB(const Napi::CallbackInfo& info);

  std::unique_ptr<bytecask::DB> db;

  // Throws if the DB has already been closed; mirrors the Embind layer's
  // explicit-delete-on-close semantics without needing GC to run.
  auto Db() -> bytecask::DB& {
    if (!db) throw std::runtime_error("Database is closed");
    return *db;
  }

 private:
  static Napi::FunctionReference constructor;

  static auto Open(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Get(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Put(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Del(const Napi::CallbackInfo& info) -> Napi::Value;
  auto DelRange(const Napi::CallbackInfo& info) -> Napi::Value;
  auto ContainsKey(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Snapshot(const Napi::CallbackInfo& info) -> Napi::Value;
  auto ApplyBatch(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Entries(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Keys(const Napi::CallbackInfo& info) -> Napi::Value;
  auto EntriesReverse(const Napi::CallbackInfo& info) -> Napi::Value;
  auto KeysReverse(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Vacuum(const Napi::CallbackInfo& info) -> Napi::Value;
  auto IsDegraded(const Napi::CallbackInfo& info) -> Napi::Value;
  auto DegradedReason(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Resume(const Napi::CallbackInfo& info) -> void;
  auto Mode(const Napi::CallbackInfo& info) -> Napi::Value;
  auto SetMode(const Napi::CallbackInfo& info) -> void;
  auto DurableSequence(const Napi::CallbackInfo& info) -> Napi::Value;
  auto CreateManifest(const Napi::CallbackInfo& info) -> Napi::Value;
  auto ChangesSince(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Ingest(const Napi::CallbackInfo& info) -> void;
  auto Stats(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Close(const Napi::CallbackInfo& info) -> void;
};

// ---------------------------------------------------------------------------
// NapiSnapshot — move-only snapshot with consumption guard, matching the
// Embind layer's JsSnapshot::check() pattern.
// ---------------------------------------------------------------------------

class NapiSnapshot : public Napi::ObjectWrap<NapiSnapshot> {
 public:
  static auto Init(Napi::Env env, Napi::Object exports) -> void;
  explicit NapiSnapshot(const Napi::CallbackInfo& info);
  static auto NewInstance(Napi::Env env, bytecask::Snapshot snap) -> Napi::Object;

  std::optional<bytecask::Snapshot> snap;

  auto Check() const -> void {
    if (!snap) throw std::runtime_error("Snapshot is closed or has been consumed by a WritePlan");
  }

 private:
  static Napi::FunctionReference constructor;

  auto Get(const Napi::CallbackInfo& info) -> Napi::Value;
  auto ContainsKey(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Entries(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Keys(const Napi::CallbackInfo& info) -> Napi::Value;
  auto EntriesReverse(const Napi::CallbackInfo& info) -> Napi::Value;
  auto KeysReverse(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Close(const Napi::CallbackInfo& info) -> void;
};

// ---------------------------------------------------------------------------
// NapiWritePlan — move-only plan with consumption guard.
// ---------------------------------------------------------------------------

class NapiWritePlan : public Napi::ObjectWrap<NapiWritePlan> {
 public:
  static auto Init(Napi::Env env, Napi::Object exports) -> void;
  explicit NapiWritePlan(const Napi::CallbackInfo& info);

  std::optional<bytecask::WritePlan> plan;

  auto Check() const -> void {
    if (!plan) throw std::runtime_error("WritePlan is closed or has already been applied");
  }

 private:
  static Napi::FunctionReference constructor;

  static auto WithSnapshot(const Napi::CallbackInfo& info) -> Napi::Value;
  static auto WithLimits(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Put(const Napi::CallbackInfo& info) -> void;
  auto Del(const Napi::CallbackInfo& info) -> void;
  auto DelRange(const Napi::CallbackInfo& info) -> void;
  auto EnsurePresent(const Napi::CallbackInfo& info) -> void;
  auto EnsureAbsent(const Napi::CallbackInfo& info) -> void;
  auto EnsureUnchanged(const Napi::CallbackInfo& info) -> void;
  auto EnsureRangeUnchanged(const Napi::CallbackInfo& info) -> void;
  auto HasSnapshot(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Close(const Napi::CallbackInfo& info) -> void;

  friend class NapiDB;  // ApplyBatch moves plan out directly.
};

// ---------------------------------------------------------------------------
// Lazy iterator wrappers — implement the JS iterator protocol via next().
// Each holds the iterator by value and advances it one step per next() call;
// laziness is preserved end-to-end (never materializes the full range).
// ---------------------------------------------------------------------------

class NapiEntryIterator : public Napi::ObjectWrap<NapiEntryIterator> {
 public:
  static auto Init(Napi::Env env, Napi::Object exports) -> void;
  explicit NapiEntryIterator(const Napi::CallbackInfo& info);
  static auto NewInstance(Napi::Env env, bytecask::EntryIterator it)
      -> Napi::Object;

 private:
  static Napi::FunctionReference constructor;
  std::optional<bytecask::EntryIterator> it_;

  auto Next(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Close(const Napi::CallbackInfo& info) -> void;
};

class NapiKeyIterator : public Napi::ObjectWrap<NapiKeyIterator> {
 public:
  static auto Init(Napi::Env env, Napi::Object exports) -> void;
  explicit NapiKeyIterator(const Napi::CallbackInfo& info);
  static auto NewInstance(Napi::Env env, bytecask::KeyIterator it) -> Napi::Object;

 private:
  static Napi::FunctionReference constructor;
  std::optional<bytecask::KeyIterator> it_;

  auto Next(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Close(const Napi::CallbackInfo& info) -> void;
};

class NapiReverseEntryIterator
    : public Napi::ObjectWrap<NapiReverseEntryIterator> {
 public:
  static auto Init(Napi::Env env, Napi::Object exports) -> void;
  explicit NapiReverseEntryIterator(const Napi::CallbackInfo& info);
  static auto NewInstance(Napi::Env env, bytecask::ReverseEntryIterator it)
      -> Napi::Object;

 private:
  static Napi::FunctionReference constructor;
  std::optional<bytecask::ReverseEntryIterator> it_;

  auto Next(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Close(const Napi::CallbackInfo& info) -> void;
};

class NapiReverseKeyIterator : public Napi::ObjectWrap<NapiReverseKeyIterator> {
 public:
  static auto Init(Napi::Env env, Napi::Object exports) -> void;
  explicit NapiReverseKeyIterator(const Napi::CallbackInfo& info);
  static auto NewInstance(Napi::Env env, bytecask::ReverseKeyIterator it)
      -> Napi::Object;

 private:
  static Napi::FunctionReference constructor;
  std::optional<bytecask::ReverseKeyIterator> it_;

  auto Next(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Close(const Napi::CallbackInfo& info) -> void;
};

class NapiChangeIterator : public Napi::ObjectWrap<NapiChangeIterator> {
 public:
  static auto Init(Napi::Env env, Napi::Object exports) -> void;
  explicit NapiChangeIterator(const Napi::CallbackInfo& info);
  static auto NewInstance(Napi::Env env, bytecask::ChangeIterator it)
      -> Napi::Object;

 private:
  static Napi::FunctionReference constructor;
  std::optional<bytecask::ChangeIterator> it_;

  auto Next(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Close(const Napi::CallbackInfo& info) -> void;
};

// ---------------------------------------------------------------------------
// NapiFileManifest — wraps FileManifest from create_manifest().
// ---------------------------------------------------------------------------

class NapiFileManifest : public Napi::ObjectWrap<NapiFileManifest> {
 public:
  static auto Init(Napi::Env env, Napi::Object exports) -> void;
  explicit NapiFileManifest(const Napi::CallbackInfo& info);
  static auto NewInstance(Napi::Env env, bytecask::FileManifest manifest)
      -> Napi::Object;

 private:
  static Napi::FunctionReference constructor;

  Napi::ObjectReference snapshot_ref_;  // owns the NapiSnapshot object
  Napi::Reference<Napi::Array> files_ref_;
  std::uint64_t through_sequence_{0};

  auto GetSnapshot(const Napi::CallbackInfo& info) -> Napi::Value;
  auto GetFiles(const Napi::CallbackInfo& info) -> Napi::Value;
  auto GetThroughSequence(const Napi::CallbackInfo& info) -> Napi::Value;
  auto Close(const Napi::CallbackInfo& info) -> void;
};

// ===========================================================================
// NapiDB implementation
// ===========================================================================

Napi::FunctionReference NapiDB::constructor;

auto NapiDB::Init(Napi::Env env, Napi::Object exports) -> void {
  auto func = DefineClass(
      env, "ByteCaskDB",
      {
          StaticMethod<&NapiDB::Open>("open"),
          InstanceMethod<&NapiDB::Get>("get"),
          InstanceMethod<&NapiDB::Put>("put"),
          InstanceMethod<&NapiDB::Del>("del"),
          InstanceMethod<&NapiDB::DelRange>("delRange"),
          InstanceMethod<&NapiDB::ContainsKey>("containsKey"),
          InstanceMethod<&NapiDB::Snapshot>("snapshot"),
          InstanceMethod<&NapiDB::ApplyBatch>("applyBatch"),
          InstanceMethod<&NapiDB::Entries>("entries"),
          InstanceMethod<&NapiDB::Keys>("keys"),
          InstanceMethod<&NapiDB::EntriesReverse>("entriesReverse"),
          InstanceMethod<&NapiDB::KeysReverse>("keysReverse"),
          InstanceMethod<&NapiDB::Vacuum>("vacuum"),
          InstanceMethod<&NapiDB::IsDegraded>("isDegraded"),
          InstanceMethod<&NapiDB::DegradedReason>("degradedReason"),
          InstanceMethod<&NapiDB::Resume>("resume"),
          InstanceMethod<&NapiDB::Mode>("mode"),
          InstanceMethod<&NapiDB::SetMode>("setMode"),
          InstanceMethod<&NapiDB::DurableSequence>("durableSequence"),
          InstanceMethod<&NapiDB::CreateManifest>("createManifest"),
          InstanceMethod<&NapiDB::ChangesSince>("changesSince"),
          InstanceMethod<&NapiDB::Ingest>("ingest"),
          InstanceMethod<&NapiDB::Stats>("stats"),
          InstanceMethod<&NapiDB::Close>("close"),
      });
  constructor = Napi::Persistent(func);
  constructor.SuppressDestruct();
  exports.Set("ByteCaskDB", func);
}

namespace {

auto parse_open_options(const Napi::Value& opts) -> bytecask::Options {
  bytecask::Options o{.recovery_threads = 4};
  if (has_prop(opts, "maxFileBytes"))
    o.max_file_bytes = static_cast<std::uint64_t>(
        opt_prop(opts, "maxFileBytes").As<Napi::Number>().DoubleValue());
  if (has_prop(opts, "failOnCrcErrors"))
    o.fail_recovery_on_crc_errors =
        opt_prop(opts, "failOnCrcErrors").As<Napi::Boolean>();
  if (has_prop(opts, "maxKeyBytes"))
    o.max_key_bytes = opt_prop(opts, "maxKeyBytes").As<Napi::Number>().Uint32Value();
  if (has_prop(opts, "maxValueBytes"))
    o.max_value_bytes =
        opt_prop(opts, "maxValueBytes").As<Napi::Number>().Uint32Value();
  if (has_prop(opts, "initialMode"))
    o.initial_mode =
        string_to_mode(opt_prop(opts, "initialMode").As<Napi::String>().Utf8Value());
  return o;
}

}  // namespace

// DB::open() returns a non-moveable bytecask::DB; direct-initializing the
// heap object from the prvalue result is guaranteed copy elision (C++17), so
// no move/copy constructor is invoked. Using make_unique here would break
// that guarantee (its forwarding parameter binds and materializes the
// prvalue into a temporary first), so allocate with `new` directly instead.
// Heap-allocating (rather than embedding DB by value) lets Close() destroy
// it deterministically — releasing the file lock without waiting for GC —
// mirroring the Embind layer's explicit `delete` in jsdb_close.
NapiDB::NapiDB(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<NapiDB>(info),
      db{new bytecask::DB(bytecask::DB::open(
          std::filesystem::path{info[0].As<Napi::String>().Utf8Value()},
          parse_open_options(opt_arg(info, 1))))} {}

// DB::open() may throw (I/O error, lock contention, CRC failure on
// recovery); node-addon-api's NAPI_CPP_EXCEPTIONS mode (enabled in
// xmake.lua) rethrows any C++ exception escaping the constructor as a JS
// Error.
auto NapiDB::Open(const Napi::CallbackInfo& info) -> Napi::Value {
  return constructor.New({info[0], opt_arg(info, 1)});
}

auto NapiDB::Get(const Napi::CallbackInfo& info) -> Napi::Value {
  auto env = info.Env();
  auto key = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  Bytes out;
  if (Db().get(ro, to_view(key), out)) return bytes_to_buffer(env, out);
  return env.Null();
}

auto NapiDB::Put(const Napi::CallbackInfo& info) -> Napi::Value {
  auto env = info.Env();
  auto key = arg_to_string(info[0]);
  auto value = arg_to_string(info[1]);
  auto wo = extract_write_options(opt_arg(info, 2));
  return commit_result_to_js(env, Db().put(wo, to_view(key), to_view(value)));
}

auto NapiDB::Del(const Napi::CallbackInfo& info) -> Napi::Value {
  auto env = info.Env();
  auto key = arg_to_string(info[0]);
  auto wo = extract_write_options(opt_arg(info, 1));
  auto result = Db().del(wo, to_view(key));
  if (!result) return env.Null();
  return commit_result_to_js(env, *result);
}

auto NapiDB::DelRange(const Napi::CallbackInfo& info) -> Napi::Value {
  auto env = info.Env();
  auto from = arg_to_string(info[0]);
  auto to = arg_to_string(info[1]);
  auto wo = extract_write_options(opt_arg(info, 2));
  return commit_result_to_js(env, Db().del_range(wo, to_view(from), to_view(to)));
}

auto NapiDB::ContainsKey(const Napi::CallbackInfo& info) -> Napi::Value {
  auto key = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  return Napi::Boolean::New(info.Env(), Db().contains_key(ro, to_view(key)));
}

auto NapiDB::Snapshot(const Napi::CallbackInfo& info) -> Napi::Value {
  return NapiSnapshot::NewInstance(info.Env(), Db().snapshot());
}

auto NapiDB::ApplyBatch(const Napi::CallbackInfo& info) -> Napi::Value {
  auto env = info.Env();
  auto* plan_wrap = NapiWritePlan::Unwrap(info[0].As<Napi::Object>());
  plan_wrap->Check();
  auto wo = extract_write_options(opt_arg(info, 1));
  auto result = Db().apply_batch(wo, std::move(*plan_wrap->plan));
  plan_wrap->plan.reset();
  if (!result) return env.Null();
  return commit_result_to_js(env, *result);
}

auto NapiDB::Entries(const Napi::CallbackInfo& info) -> Napi::Value {
  auto from = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  auto range = Db().iter_from(ro, to_view(from));
  return NapiEntryIterator::NewInstance(info.Env(), range.begin());
}

auto NapiDB::Keys(const Napi::CallbackInfo& info) -> Napi::Value {
  auto from = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  auto range = Db().keys_from(ro, to_view(from));
  return NapiKeyIterator::NewInstance(info.Env(), range.begin());
}

auto NapiDB::EntriesReverse(const Napi::CallbackInfo& info) -> Napi::Value {
  auto from = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  auto range = Db().riter_from(ro, to_view(from));
  return NapiReverseEntryIterator::NewInstance(info.Env(), range.begin());
}

auto NapiDB::KeysReverse(const Napi::CallbackInfo& info) -> Napi::Value {
  auto from = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  auto range = Db().rkeys_from(ro, to_view(from));
  return NapiReverseKeyIterator::NewInstance(info.Env(), range.begin());
}

auto NapiDB::Vacuum(const Napi::CallbackInfo& info) -> Napi::Value {
  return Napi::Boolean::New(info.Env(), Db().vacuum());
}

auto NapiDB::IsDegraded(const Napi::CallbackInfo& info) -> Napi::Value {
  return Napi::Boolean::New(info.Env(), Db().is_degraded());
}

auto NapiDB::DegradedReason(const Napi::CallbackInfo& info) -> Napi::Value {
  return Napi::String::New(info.Env(), Db().degraded_reason());
}

auto NapiDB::Resume(const Napi::CallbackInfo&) -> void { Db().resume(); }

auto NapiDB::Mode(const Napi::CallbackInfo& info) -> Napi::Value {
  return Napi::String::New(info.Env(), mode_to_string(Db().mode()));
}

auto NapiDB::SetMode(const Napi::CallbackInfo& info) -> void {
  Db().set_mode(string_to_mode(info[0].As<Napi::String>().Utf8Value()));
}

// The single sequence primitive. minSequence/timeoutMs default to 0 (an
// immediate, non-blocking poll) when omitted from JS. A positive timeoutMs
// blocks the calling thread — the Node event loop in this synchronous
// binding — until the durable sequence reaches minSequence or the timeout
// expires (see docs/native_node_binding_design.md "Concurrency model").
auto NapiDB::DurableSequence(const Napi::CallbackInfo& info) -> Napi::Value {
  auto min_sequence = opt_bigint_arg(info, 0);
  auto timeout_ms = opt_uint32_arg(info, 1);
  auto result = Db().durable_sequence(min_sequence, std::chrono::milliseconds{timeout_ms});
  return Napi::BigInt::New(info.Env(), result);
}

auto NapiDB::CreateManifest(const Napi::CallbackInfo& info) -> Napi::Value {
  return NapiFileManifest::NewInstance(info.Env(), Db().create_manifest());
}

auto NapiDB::ChangesSince(const Napi::CallbackInfo& info) -> Napi::Value {
  auto* snap_wrap = NapiSnapshot::Unwrap(info[0].As<Napi::Object>());
  snap_wrap->Check();
  bool lossless = false;
  auto from_seq = info[1].As<Napi::BigInt>().Uint64Value(&lossless);
  auto range = Db().changes_since(*snap_wrap->snap, from_seq);
  return NapiChangeIterator::NewInstance(info.Env(), range.begin());
}

auto NapiDB::Ingest(const Napi::CallbackInfo& info) -> void {
  auto entries = info[0].As<Napi::Array>();
  auto len = entries.Length();

  std::vector<bytecask::DataEntryView> views;
  views.reserve(len);
  // Keep owned string buffers alive for the duration of ingest — the views
  // above borrow from them.
  std::vector<std::string> key_bufs;
  std::vector<std::string> val_bufs;
  key_bufs.reserve(len);
  val_bufs.reserve(len);

  for (std::uint32_t i = 0; i < len; ++i) {
    auto e = entries.Get(i).As<Napi::Object>();
    bool lossless = false;
    auto seq = e.Get("sequence").As<Napi::BigInt>().Uint64Value(&lossless);
    auto et = string_to_entry_type(e.Get("entryType").As<Napi::String>().Utf8Value());

    key_bufs.push_back(arg_to_string(e.Get("key")));
    val_bufs.push_back(arg_to_string(e.Get("value")));

    views.push_back(bytecask::DataEntryView{
        .sequence = seq,
        .entry_type = et,
        .key = to_view(key_bufs.back()),
        .value = to_view(val_bufs.back()),
    });
  }
  Db().ingest(views);
}

auto NapiDB::Stats(const Napi::CallbackInfo& info) -> Napi::Value {
  auto env = info.Env();
  auto stats = Db().stats();
  auto result = Napi::Object::New(env);
  for (auto& [key, value] : stats) {
    result.Set(key, Napi::Number::New(env, static_cast<double>(value)));
  }
  return result;
}

auto NapiDB::Close(const Napi::CallbackInfo&) -> void {
  // Deterministically destroys the wrapped bytecask::DB (releasing its file
  // lock and flushing) rather than waiting for GC — matches the Embind
  // layer's explicit `delete` in jsdb_close. Idempotent: db is null if
  // close() has already run.
  db.reset();
}

// ===========================================================================
// NapiSnapshot implementation
// ===========================================================================

Napi::FunctionReference NapiSnapshot::constructor;

auto NapiSnapshot::Init(Napi::Env env, Napi::Object exports) -> void {
  auto func = DefineClass(
      env, "Snapshot",
      {
          InstanceMethod<&NapiSnapshot::Get>("get"),
          InstanceMethod<&NapiSnapshot::ContainsKey>("containsKey"),
          InstanceMethod<&NapiSnapshot::Entries>("entries"),
          InstanceMethod<&NapiSnapshot::Keys>("keys"),
          InstanceMethod<&NapiSnapshot::EntriesReverse>("entriesReverse"),
          InstanceMethod<&NapiSnapshot::KeysReverse>("keysReverse"),
          InstanceMethod<&NapiSnapshot::Close>("close"),
      });
  constructor = Napi::Persistent(func);
  constructor.SuppressDestruct();
  exports.Set("Snapshot", func);
}

NapiSnapshot::NapiSnapshot(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<NapiSnapshot>(info) {
  auto* holder = info[0].As<Napi::External<bytecask::Snapshot>>().Data();
  snap.emplace(std::move(*holder));
  delete holder;
}

auto NapiSnapshot::NewInstance(Napi::Env env, bytecask::Snapshot s) -> Napi::Object {
  auto* holder = new bytecask::Snapshot{std::move(s)};
  auto external = Napi::External<bytecask::Snapshot>::New(env, holder);
  return constructor.New({external});
}

auto NapiSnapshot::Get(const Napi::CallbackInfo& info) -> Napi::Value {
  Check();
  auto env = info.Env();
  auto key = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  Bytes out;
  if (snap->get(ro, to_view(key), out)) return bytes_to_buffer(env, out);
  return env.Null();
}

auto NapiSnapshot::ContainsKey(const Napi::CallbackInfo& info) -> Napi::Value {
  Check();
  auto key = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  return Napi::Boolean::New(info.Env(), snap->contains_key(ro, to_view(key)));
}

auto NapiSnapshot::Entries(const Napi::CallbackInfo& info) -> Napi::Value {
  Check();
  auto from = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  auto range = snap->iter_from(ro, to_view(from));
  return NapiEntryIterator::NewInstance(info.Env(), range.begin());
}

auto NapiSnapshot::Keys(const Napi::CallbackInfo& info) -> Napi::Value {
  Check();
  auto from = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  auto range = snap->keys_from(ro, to_view(from));
  return NapiKeyIterator::NewInstance(info.Env(), range.begin());
}

auto NapiSnapshot::EntriesReverse(const Napi::CallbackInfo& info) -> Napi::Value {
  Check();
  auto from = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  auto range = snap->riter_from(ro, to_view(from));
  return NapiReverseEntryIterator::NewInstance(info.Env(), range.begin());
}

auto NapiSnapshot::KeysReverse(const Napi::CallbackInfo& info) -> Napi::Value {
  Check();
  auto from = arg_to_string(info[0]);
  auto ro = extract_read_options(opt_arg(info, 1));
  auto range = snap->rkeys_from(ro, to_view(from));
  return NapiReverseKeyIterator::NewInstance(info.Env(), range.begin());
}

auto NapiSnapshot::Close(const Napi::CallbackInfo&) -> void {
  snap.reset();
}

// ===========================================================================
// NapiWritePlan implementation
// ===========================================================================

Napi::FunctionReference NapiWritePlan::constructor;

auto NapiWritePlan::Init(Napi::Env env, Napi::Object exports) -> void {
  auto func = DefineClass(
      env, "WritePlan",
      {
          StaticMethod<&NapiWritePlan::WithSnapshot>("withSnapshot"),
          StaticMethod<&NapiWritePlan::WithLimits>("withLimits"),
          InstanceMethod<&NapiWritePlan::Put>("put"),
          InstanceMethod<&NapiWritePlan::Del>("del"),
          InstanceMethod<&NapiWritePlan::DelRange>("delRange"),
          InstanceMethod<&NapiWritePlan::EnsurePresent>("ensurePresent"),
          InstanceMethod<&NapiWritePlan::EnsureAbsent>("ensureAbsent"),
          InstanceMethod<&NapiWritePlan::EnsureUnchanged>("ensureUnchanged"),
          InstanceMethod<&NapiWritePlan::EnsureRangeUnchanged>(
              "ensureRangeUnchanged"),
          InstanceMethod<&NapiWritePlan::HasSnapshot>("hasSnapshot"),
          InstanceMethod<&NapiWritePlan::Close>("close"),
      });
  constructor = Napi::Persistent(func);
  constructor.SuppressDestruct();
  exports.Set("WritePlan", func);
}

// Plain `new WritePlan()` (no external token argument) constructs an
// unconditional, snapshot-less plan — mirrors JsWritePlan's default
// constructor in the Embind layer.
NapiWritePlan::NapiWritePlan(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<NapiWritePlan>(info) {
  if (info.Length() > 0 && info[0].IsExternal()) {
    auto* holder = info[0].As<Napi::External<bytecask::WritePlan>>().Data();
    plan.emplace(std::move(*holder));
    delete holder;
    return;
  }
  plan.emplace(bytecask::WritePlan{});
}

auto NapiWritePlan::WithSnapshot(const Napi::CallbackInfo& info) -> Napi::Value {
  auto env = info.Env();
  auto* snap_wrap = NapiSnapshot::Unwrap(info[0].As<Napi::Object>());
  snap_wrap->Check();
  auto* holder =
      new bytecask::WritePlan{bytecask::WritePlan{std::move(*snap_wrap->snap)}};
  snap_wrap->snap.reset();
  auto external = Napi::External<bytecask::WritePlan>::New(env, holder);
  return constructor.New({external});
}

auto NapiWritePlan::WithLimits(const Napi::CallbackInfo& info) -> Napi::Value {
  auto env = info.Env();
  auto opts = opt_arg(info, 0);
  bytecask::SizeLimits limits;
  if (has_prop(opts, "maxKeyBytes"))
    limits.max_key_bytes =
        opt_prop(opts, "maxKeyBytes").As<Napi::Number>().Uint32Value();
  if (has_prop(opts, "maxValueBytes"))
    limits.max_value_bytes =
        opt_prop(opts, "maxValueBytes").As<Napi::Number>().Uint32Value();
  auto* holder = new bytecask::WritePlan{bytecask::WritePlan{limits}};
  auto external = Napi::External<bytecask::WritePlan>::New(env, holder);
  return constructor.New({external});
}

auto NapiWritePlan::Put(const Napi::CallbackInfo& info) -> void {
  Check();
  auto key = arg_to_string(info[0]);
  auto value = arg_to_string(info[1]);
  plan->put(to_view(key), to_view(value));
}

auto NapiWritePlan::Del(const Napi::CallbackInfo& info) -> void {
  Check();
  auto key = arg_to_string(info[0]);
  plan->del(to_view(key));
}

auto NapiWritePlan::DelRange(const Napi::CallbackInfo& info) -> void {
  Check();
  auto from = arg_to_string(info[0]);
  auto to = arg_to_string(info[1]);
  plan->del_range(to_view(from), to_view(to));
}

auto NapiWritePlan::EnsurePresent(const Napi::CallbackInfo& info) -> void {
  Check();
  auto key = arg_to_string(info[0]);
  plan->ensure_present(to_view(key));
}

auto NapiWritePlan::EnsureAbsent(const Napi::CallbackInfo& info) -> void {
  Check();
  auto key = arg_to_string(info[0]);
  plan->ensure_absent(to_view(key));
}

auto NapiWritePlan::EnsureUnchanged(const Napi::CallbackInfo& info) -> void {
  Check();
  auto key = arg_to_string(info[0]);
  plan->ensure_unchanged(to_view(key));
}

auto NapiWritePlan::EnsureRangeUnchanged(const Napi::CallbackInfo& info) -> void {
  Check();
  auto from = arg_to_string(info[0]);
  auto to = arg_to_string(info[1]);
  plan->ensure_range_unchanged(to_view(from), to_view(to));
}

auto NapiWritePlan::HasSnapshot(const Napi::CallbackInfo& info) -> Napi::Value {
  Check();
  return Napi::Boolean::New(info.Env(), plan->has_snapshot());
}

auto NapiWritePlan::Close(const Napi::CallbackInfo&) -> void { plan.reset(); }

// ===========================================================================
// Iterator wrappers implementation
// ===========================================================================

#define BC_DEFINE_ITERATOR(ClassName, JsName, EngineIter, NextBody)          \
  Napi::FunctionReference ClassName::constructor;                            \
                                                                               \
  auto ClassName::Init(Napi::Env env, Napi::Object exports) -> void {        \
    auto func = DefineClass(env, JsName,                                     \
                             {                                                \
                                 InstanceMethod<&ClassName::Next>("next"),    \
                                 InstanceMethod<&ClassName::Close>("close"),  \
                             });                                              \
    constructor = Napi::Persistent(func);                                    \
    constructor.SuppressDestruct();                                          \
    exports.Set(JsName, func);                                                \
  }                                                                           \
                                                                               \
  ClassName::ClassName(const Napi::CallbackInfo& info)                       \
      : Napi::ObjectWrap<ClassName>(info) {                                  \
    auto* holder = info[0].As<Napi::External<EngineIter>>().Data();          \
    it_.emplace(std::move(*holder));                                         \
    delete holder;                                                           \
  }                                                                           \
                                                                               \
  auto ClassName::NewInstance(Napi::Env env, EngineIter it) -> Napi::Object { \
    auto* holder = new EngineIter{std::move(it)};                            \
    auto external = Napi::External<EngineIter>::New(env, holder);            \
    return constructor.New({external});                                      \
  }                                                                           \
                                                                               \
  auto ClassName::Close(const Napi::CallbackInfo&) -> void { it_.reset(); }   \
                                                                               \
  auto ClassName::Next(const Napi::CallbackInfo& info) -> Napi::Value {      \
    auto env = info.Env();                                                   \
    auto result = Napi::Object::New(env);                                    \
    if (!it_ || *it_ == std::default_sentinel) {                             \
      result.Set("done", Napi::Boolean::New(env, true));                     \
      return result;                                                         \
    }                                                                        \
    NextBody                                                                 \
    result.Set("done", Napi::Boolean::New(env, false));                      \
    ++(*it_);                                                                \
    return result;                                                           \
  }

BC_DEFINE_ITERATOR(NapiEntryIterator, "EntryIterator", bytecask::EntryIterator, {
  const auto& e = **it_;
  auto entry = Napi::Object::New(env);
  entry.Set("key", span_to_buffer(env, e.key));
  entry.Set("value", span_to_buffer(env, e.value));
  result.Set("value", entry);
})

BC_DEFINE_ITERATOR(NapiKeyIterator, "KeyIterator", bytecask::KeyIterator, {
  result.Set("value", bytes_to_buffer(env, **it_));
})

BC_DEFINE_ITERATOR(NapiReverseEntryIterator, "ReverseEntryIterator", bytecask::ReverseEntryIterator, {
  const auto& e = **it_;
  auto entry = Napi::Object::New(env);
  entry.Set("key", span_to_buffer(env, e.key));
  entry.Set("value", span_to_buffer(env, e.value));
  result.Set("value", entry);
})

BC_DEFINE_ITERATOR(NapiReverseKeyIterator, "ReverseKeyIterator", bytecask::ReverseKeyIterator, {
  result.Set("value", bytes_to_buffer(env, **it_));
})

BC_DEFINE_ITERATOR(NapiChangeIterator, "ChangeIterator", bytecask::ChangeIterator, {
  const auto& view = **it_;
  auto entry = Napi::Object::New(env);
  entry.Set("sequence", Napi::BigInt::New(env, view.sequence));
  entry.Set("entryType",
            Napi::String::New(env, entry_type_to_string(view.entry_type)));
  entry.Set("key", span_to_buffer(env, view.key));
  entry.Set("value", span_to_buffer(env, view.value));
  result.Set("value", entry);
})

#undef BC_DEFINE_ITERATOR

// ===========================================================================
// NapiFileManifest implementation
// ===========================================================================

Napi::FunctionReference NapiFileManifest::constructor;

auto NapiFileManifest::Init(Napi::Env env, Napi::Object exports) -> void {
  auto func = DefineClass(
      env, "FileManifest",
      {
          InstanceMethod<&NapiFileManifest::GetSnapshot>("getSnapshot"),
          InstanceMethod<&NapiFileManifest::GetFiles>("getFiles"),
          InstanceMethod<&NapiFileManifest::GetThroughSequence>(
              "getThroughSequence"),
          InstanceMethod<&NapiFileManifest::Close>("close"),
      });
  constructor = Napi::Persistent(func);
  constructor.SuppressDestruct();
  exports.Set("FileManifest", func);
}

// Constructed only from NewInstance (via bytecask::FileManifest passed as an
// External); the JS-facing constructor is never called directly by users.
NapiFileManifest::NapiFileManifest(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<NapiFileManifest>(info) {
  auto env = info.Env();
  auto* holder =
      info[0].As<Napi::External<bytecask::FileManifest>>().Data();
  auto manifest = std::move(*holder);
  delete holder;

  auto snap_obj = NapiSnapshot::NewInstance(env, std::move(manifest.snap));
  snapshot_ref_ = Napi::Persistent(snap_obj);

  auto js_files = Napi::Array::New(env, manifest.files.size());
  for (std::size_t i = 0; i < manifest.files.size(); ++i) {
    auto& fi = manifest.files[i];
    auto obj = Napi::Object::New(env);
    obj.Set("fileId", Napi::Number::New(env, fi.file_id));
    obj.Set("dataPath", Napi::String::New(env, fi.data_path.string()));
    obj.Set("hintPath", Napi::String::New(env, fi.hint_path.string()));
    js_files.Set(static_cast<std::uint32_t>(i), obj);
  }
  files_ref_ = Napi::Persistent(js_files);
  through_sequence_ = manifest.through_sequence;
}

auto NapiFileManifest::NewInstance(Napi::Env env, bytecask::FileManifest manifest)
    -> Napi::Object {
  auto* holder = new bytecask::FileManifest{std::move(manifest)};
  auto external = Napi::External<bytecask::FileManifest>::New(env, holder);
  return constructor.New({external});
}

auto NapiFileManifest::GetSnapshot(const Napi::CallbackInfo&) -> Napi::Value {
  return snapshot_ref_.Value();
}

auto NapiFileManifest::GetFiles(const Napi::CallbackInfo&) -> Napi::Value {
  return files_ref_.Value();
}

auto NapiFileManifest::GetThroughSequence(const Napi::CallbackInfo& info)
    -> Napi::Value {
  return Napi::BigInt::New(info.Env(), through_sequence_);
}

auto NapiFileManifest::Close(const Napi::CallbackInfo&) -> void {
  // snapshot is owned separately (via snapshot_ref_ -> NapiSnapshot); the
  // caller is responsible for closing it, matching the Embind layer.
  snapshot_ref_.Reset();
  files_ref_.Reset();
}

// ===========================================================================
// Module init
// ===========================================================================

auto InitAll(Napi::Env env, Napi::Object exports) -> Napi::Object {
  NapiDB::Init(env, exports);
  NapiSnapshot::Init(env, exports);
  NapiWritePlan::Init(env, exports);
  NapiEntryIterator::Init(env, exports);
  NapiKeyIterator::Init(env, exports);
  NapiReverseEntryIterator::Init(env, exports);
  NapiReverseKeyIterator::Init(env, exports);
  NapiChangeIterator::Init(env, exports);
  NapiFileManifest::Init(env, exports);
  return exports;
}

NODE_API_MODULE(bytecask, InitAll)
