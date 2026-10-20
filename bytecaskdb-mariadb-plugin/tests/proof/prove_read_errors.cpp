// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// prove_read_errors.cpp — a read that fails is an engine error, never
// "absent" or "not found" (#524). A duplicate check that took it as absent
// lets the INSERT, UPDATE or ALTER bulk copy write over the row it could not
// read; a row fetch that took it as not found lets an UPDATE or DELETE skip
// the row.
//
// Each test damages one committed record on disk, so a checksum-verified
// read of it fails, then runs the statement and checks that it fails with
// HA_ERR_GENERIC and that the commit after it writes nothing.

#include "harness.h"
#include "bytecask_view.h"
#include "key_encoding.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>

namespace bytecaskdb {
extern std::atomic<bool> g_verify_checksums;
}

using namespace bytecaskdb;
using namespace bytecaskdb::testing;

namespace {

TableSpec plain_spec() { return TableSpec{}; }  // two columns, PK on the first

// Three columns, PK on the first, an index on the second.
TableSpec indexed_spec(bool unique) {
  TableSpec spec;
  spec.num_int_columns = 3;
  spec.secondary_indexes.push_back(IndexSpec{
      /*index_id=*/1, /*key_parts=*/1, /*is_unique=*/unique,
      /*column_indexes=*/{1}});
  return spec;
}

// Flips the byte `at` bytes into the first occurrence of record (a record's
// key followed by its value) in the data files.
void flip(const std::filesystem::path &dir, const std::vector<char> &record,
          std::ptrdiff_t at) {
  for (const auto &e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() != ".data") continue;
    std::vector<char> bytes;
    {
      std::ifstream in(e.path(), std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    auto it = std::search(bytes.begin(), bytes.end(), record.begin(), record.end());
    if (it == bytes.end()) continue;
    const auto pos = (it - bytes.begin()) + at;
    std::fstream f(e.path(), std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(pos);
    f.put(static_cast<char>(bytes[static_cast<std::size_t>(pos)] ^ 0xFF));
    return;
  }
  FAIL("record not found in any data file");
}

std::vector<char> record_bytes(const std::vector<uint8_t> &key,
                               const bytecask::Bytes &value) {
  std::vector<char> out(key.begin(), key.end());
  for (auto b : value) out.push_back(static_cast<char>(b));
  return out;
}

// The active file is zero-filled ahead of its cursor, so its size does not
// show an append; the engine's counter does.
std::int64_t bytes_written(bytecask::DB &db) {
  return db.stats().at("bytecask.bytes_written");
}

// Commits rows, then damages one record. Its value is kept to show, read
// without the check, that the record on disk is still the one committed.
struct Damaged {
  PluginTestHarness h;
  std::vector<uint8_t> key;
  bytecask::Bytes value;

  Damaged(TableSpec spec, const std::vector<std::vector<int32_t>> &rows)
      : h{std::move(spec)} {
    for (const auto &r : rows) REQUIRE(h.insert_row(r) == 0);
    REQUIRE(h.commit() == 0);
  }

  void damage_row(const std::vector<int32_t> &row) {
    auto k = h.pk_key(row);
    key.assign(k.begin(), k.end());
    damage_crc();
  }

  // The table's n-th entry in secondary index 1.
  void damage_index_entry(int n = 0) {
    auto lo = index_id_prefix(h.table_id(), 1);
    auto it = h.db().keys_from({}, as_view(lo)).begin();
    for (int i = 0; i < n; ++i) ++it;
    key.assign(u8_data(*it), u8_data(*it) + (*it).size());
    damage_crc();
  }

  // Flips the first byte of the record's trailing CRC: every verified read
  // of it fails.
  void damage_crc() {
    REQUIRE(h.db().get({}, as_view(key), value));
    flip(h.path(), record_bytes(key, value),
         std::ssize(key) + std::ssize(value));
    bytecask::Bytes out;
    REQUIRE_THROWS(h.db().get({}, as_view(key), out));
  }

  void check_unchanged() {
    bytecask::Bytes out;
    REQUIRE(h.db().get({.verify_checksums = false}, as_view(key), out));
    CHECK(out == value);
  }
};

// Runs a failing statement, then commits what the transaction holds: had the
// failed read been taken as absent, the commit is where the overwrite lands.
void check_refused_and_nothing_written(Damaged &d, const std::function<int()> &stmt) {
  const auto before = bytes_written(d.h.db());
  CHECK(stmt() == HA_ERR_GENERIC);
  CHECK(d.h.commit() == 0);
  CHECK(bytes_written(d.h.db()) == before);
}

}  // namespace

TEST_CASE("P-READ-1: INSERT whose duplicate check fails to read is refused",
          "[proof][read_errors]") {
  Damaged d(plain_spec(), {{7, 70}});
  d.damage_row({7, 70});
  check_refused_and_nothing_written(d, [&] { return d.h.insert_row({7, 99}); });
  d.check_unchanged();
}

TEST_CASE("P-READ-2: bulk copy whose duplicate check fails to read is refused",
          "[proof][read_errors][bulk]") {
  Damaged d(plain_spec(), {{7, 70}});
  d.damage_row({7, 70});
  d.h.txn().begin_bulk_copy(1 << 20);
  check_refused_and_nothing_written(d, [&] {
    const int rc = d.h.insert_row({7, 99});
    CHECK(d.h.handler().end_bulk_insert() == 0);
    return rc;
  });
  d.check_unchanged();
}

TEST_CASE("P-READ-3: UPDATE onto a key whose duplicate check fails to read is refused",
          "[proof][read_errors][update]") {
  Damaged d(plain_spec(), {{7, 70}, {8, 80}});
  d.damage_row({7, 70});
  check_refused_and_nothing_written(d, [&] { return d.h.update_row({8, 80}, {7, 80}); });
  d.check_unchanged();
}

TEST_CASE("P-READ-4: INSERT whose unique index check fails to read is refused",
          "[proof][read_errors][unique]") {
  Damaged d(indexed_spec(/*unique=*/true), {{7, 70, 0}});
  d.damage_index_entry();
  check_refused_and_nothing_written(d, [&] { return d.h.insert_row({8, 70, 0}); });
  d.check_unchanged();
}

TEST_CASE("P-READ-5: UPDATE whose unique index check fails to read is refused",
          "[proof][read_errors][unique][update]") {
  Damaged d(indexed_spec(/*unique=*/true), {{7, 70, 0}, {8, 80, 0}});
  d.damage_index_entry();
  check_refused_and_nothing_written(d, [&] {
    return d.h.update_row({8, 80, 0}, {8, 70, 0});
  });
  d.check_unchanged();
}

TEST_CASE("P-READ-6: bulk copy whose unique index check fails to read is refused",
          "[proof][read_errors][unique][bulk]") {
  Damaged d(indexed_spec(/*unique=*/true), {{7, 70, 0}});
  d.damage_index_entry();
  d.h.txn().begin_bulk_copy(1 << 20);
  check_refused_and_nothing_written(d, [&] {
    const int rc = d.h.insert_row({8, 70, 0});
    CHECK(d.h.handler().end_bulk_insert() == 0);
    return rc;
  });
  d.check_unchanged();
}

TEST_CASE("P-READ-7: a row an index names but that cannot be read is an error",
          "[proof][read_errors][index]") {
  Damaged d(indexed_spec(/*unique=*/false), {{7, 70, 0}});
  d.damage_row({7, 70, 0});
  std::vector<uchar> buf(64, 0);
  REQUIRE(d.h.handler().index_init(1, true) == 0);
  // Not HA_ERR_KEY_NOT_FOUND: an UPDATE or DELETE would skip the row.
  CHECK(d.h.handler().index_first(buf.data()) == HA_ERR_GENERIC);
  (void)d.h.handler().index_end();
}

TEST_CASE("P-READ-8: a duplicate check verifies checksums whatever the setting says",
          "[proof][read_errors][verify]") {
  struct VerifyOff {
    VerifyOff() { g_verify_checksums.store(false); }
    ~VerifyOff() { g_verify_checksums.store(true); }
  } off;
  Damaged d(plain_spec(), {{7, 70}});
  d.key = d.h.pk_key({7, 70});
  REQUIRE(d.h.db().get({}, as_view(d.key), d.value));
  // Damage the key's last byte: read without the check, the record holds
  // another key, so the probe would find 7 absent and write over it.
  flip(d.h.path(), record_bytes(d.key, d.value), std::ssize(d.key) - 1);
  CHECK_FALSE(d.h.db().contains_key({.verify_checksums = false}, as_view(d.key)));
  check_refused_and_nothing_written(d, [&] { return d.h.insert_row({7, 99}); });
}

TEST_CASE("P-READ-9: CHECK TABLE reports an index entry it cannot read as corrupt",
          "[proof][read_errors][check]") {
  // The second entry: a table scan reads one key past the rows, the first
  // index entry, and a scan's read failure is outside this check.
  Damaged d(indexed_spec(/*unique=*/false), {{7, 70, 0}, {8, 80, 0}});
  d.damage_index_entry(1);
  CHECK(d.h.handler().check(nullptr, nullptr) == HA_ADMIN_CORRUPT);
}
