// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// prove_read_errors.cpp — a duplicate check whose read fails is an engine
// error, never "absent" (#524). Taking it as absent lets the INSERT, or the
// ALTER bulk copy, write over the row it could not read.

#include "harness.h"
#include "bytecask_view.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>

using namespace bytecaskdb;
using namespace bytecaskdb::testing;

namespace {

auto data_files(const std::filesystem::path &dir) {
  std::vector<std::filesystem::path> out;
  for (const auto &e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() == ".data") out.push_back(e.path());
  }
  return out;
}

// The active file is zero-filled ahead of its cursor, so its size does not
// show an append; the engine's counter does.
std::int64_t bytes_written(bytecask::DB &db) {
  return db.stats().at("bytecask.bytes_written");
}

// Flips the first byte of the trailing CRC of the record holding key and
// value, so every checked read of it fails.
void corrupt_record(const std::filesystem::path &dir,
                    const std::vector<uchar> &key, const bytecask::Bytes &value) {
  std::vector<char> needle(key.begin(), key.end());
  for (auto b : value) needle.push_back(static_cast<char>(b));
  for (const auto &p : data_files(dir)) {
    std::vector<char> bytes;
    {
      std::ifstream in(p, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    auto it = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
    if (it == bytes.end()) continue;
    const auto crc_at = (it - bytes.begin()) + std::ssize(needle);
    std::fstream f(p, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(crc_at);
    f.put(static_cast<char>(bytes[static_cast<std::size_t>(crc_at)] ^ 0xFF));
    return;
  }
  FAIL("record not found in any data file");
}

// Commits row {7, 70}, then damages its record on disk.
struct DamagedRow {
  PluginTestHarness h{TableSpec{}};
  std::vector<uchar> key;
  bytecask::Bytes value;

  DamagedRow() {
    REQUIRE(h.insert_row({7, 70}) == 0);
    REQUIRE(h.commit() == 0);
    key = h.pk_key({7, 70});
    REQUIRE(h.db().get({}, as_view(key.data(), key.size()), value));
    corrupt_record(h.path(), key, value);
    bytecask::Bytes out;
    REQUIRE_THROWS(h.db().get({}, as_view(key.data(), key.size()), out));
  }

  // The stored record is still the one committed: read without the check,
  // it holds the original value.
  void check_unchanged() {
    bytecask::Bytes out;
    REQUIRE(h.db().get({.verify_checksums = false},
                       as_view(key.data(), key.size()), out));
    CHECK(out == value);
  }
};

}  // namespace

TEST_CASE("P-READ-1: INSERT whose duplicate check fails to read is refused",
          "[proof][read_errors]") {
  DamagedRow r;
  const auto before = bytes_written(r.h.db());

  CHECK(r.h.insert_row({7, 99}) == HA_ERR_INTERNAL_ERROR);
  // Commit what the transaction holds: had the row been taken as absent,
  // this is where it would be overwritten.
  CHECK(r.h.commit() == 0);

  CHECK(bytes_written(r.h.db()) == before);
  r.check_unchanged();
}

TEST_CASE("P-READ-2: bulk copy whose duplicate check fails to read is refused",
          "[proof][read_errors][bulk]") {
  DamagedRow r;
  const auto before = bytes_written(r.h.db());

  r.h.txn().begin_bulk_copy(1 << 20);
  CHECK(r.h.insert_row({7, 99}) == HA_ERR_INTERNAL_ERROR);
  CHECK(r.h.handler().end_bulk_insert() == 0);
  CHECK(r.h.commit() == 0);

  CHECK(bytes_written(r.h.db()) == before);
  r.check_unchanged();
}
