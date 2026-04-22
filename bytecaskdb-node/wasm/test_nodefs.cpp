// Single-threaded NODEFS smoke test for ByteCaskDB under Emscripten.
// NODEFS maps a host directory into the Emscripten virtual filesystem
// using Node.js synchronous fs APIs. No pthreads required.

#include <cassert>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <emscripten.h>

import bytecask;

using namespace bytecask;

auto to_bytes(std::string_view sv) -> BytesView {
  return std::as_bytes(std::span{sv.data(), sv.size()});
}

auto to_string(const Bytes& b) -> std::string {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

int main() {
  namespace fs = std::filesystem;

  try {

  // Mount the current working directory at /host via NODEFS.
  EM_ASM(
    FS.mkdir('/host');
    FS.mount(NODEFS, { root: '.' }, '/host');
  );
  fprintf(stderr, "NODEFS mounted at /host\n");

  const auto dir = fs::path("/host/bytecask_nodefs_test");
  fs::remove_all(dir);
  fs::create_directories(dir);

  // Phase 1: write data
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
    fprintf(stderr, "Reads OK. Closing DB...\n");
  }

  // Phase 2: reopen and verify recovery
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
