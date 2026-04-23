// WasmFS smoke test: mount node backend, then open/put/get/close/recover.
#include <emscripten/wasmfs.h>

import bytecask;

#include <cassert>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

using namespace bytecask;

auto to_bytes(std::string_view sv) -> BytesView {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

auto to_string(const Bytes& b) -> std::string {
  return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

int main() {
  namespace fs = std::filesystem;

  try {

  // Mount the host filesystem at /host via WasmFS node backend
  // Mount the host filesystem at /host via WasmFS node backend
  auto backend = wasmfs_create_node_backend(".");
  int rc = wasmfs_create_directory("/host", 0777, backend);
  if (rc != 0) {
    fprintf(stderr, "Failed to mount node backend at /host\n");
    return 1;
  }
  fprintf(stderr, "WasmFS node backend mounted at /host\n");

  const auto dir = fs::path("/host/bytecask_wasmfs_test");
  fs::remove_all(dir);

  fprintf(stderr, "Opening DB...\n");
  {
    auto db = DB::open(dir, {.recovery_threads = 1});

    fprintf(stderr, "Writing keys...\n");
    WriteOptions wo{.sync = false};
    db.put(wo, to_bytes("hello"), to_bytes("world"));
    db.put(wo, to_bytes("foo"), to_bytes("bar"));
    db.put(wo, to_bytes("num"), to_bytes("42"));

    fprintf(stderr, "Reading keys...\n");
    Bytes out;
    assert(db.get({}, to_bytes("hello"), out));
    assert(to_string(out) == "world");
    assert(db.get({}, to_bytes("foo"), out));
    assert(to_string(out) == "bar");
    assert(db.get({}, to_bytes("num"), out));
    assert(to_string(out) == "42");
    assert(!db.get({}, to_bytes("missing"), out));
    fprintf(stderr, "Reads OK. Closing DB...\n");
  }

  fprintf(stderr, "Reopening DB (recovery)...\n");
  {
    auto db = DB::open(dir, {.recovery_threads = 1});

    Bytes out;
    assert(db.get({}, to_bytes("hello"), out));
    assert(to_string(out) == "world");
    assert(db.get({}, to_bytes("foo"), out));
    assert(to_string(out) == "bar");
    assert(db.get({}, to_bytes("num"), out));
    assert(to_string(out) == "42");
    fprintf(stderr, "Recovery OK.\n");
  }

  fs::remove_all(dir);
  fprintf(stderr, "PASS\n");
  return 0;
  } catch (const std::exception& e) {
    fprintf(stderr, "FATAL exception: %s\n", e.what());
    return 1;
  }
}
