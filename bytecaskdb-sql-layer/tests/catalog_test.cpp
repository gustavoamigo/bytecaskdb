// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/bytecask.hpp"

import bytecask.sql.catalog;
import bytecask.sql.tests.util;

using namespace bytecask::sql;
using sql_test::bytes;

namespace {

auto create_t(Catalog& cat, bytecask::DB& db) -> const Table& {
  return cat.create_table(db, "t",
                          {{"id", PhysType::I64, true}, {"name", PhysType::Bytes, true}, {"qty", PhysType::I64, true}},
                          {"id"});
}

auto constant(const std::vector<std::pair<std::string, std::uint32_t>>& cs, std::string_view n) -> std::uint32_t {
  auto it = std::ranges::find(cs, n, &std::pair<std::string, std::uint32_t>::first);
  REQUIRE(it != cs.end());
  return it->second;
}

}  // namespace

TEST_CASE("catalog: CREATE TABLE and CREATE INDEX persist and reload", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog"};
  std::uint64_t version = 0;
  {
    auto db = bytecask::DB::open(dir.path());
    auto cat = Catalog::load(db);
    CHECK(cat.version() == 0);
    const auto& t = create_t(cat, db);
    CHECK(t.id == 1);
    CHECK(t.layout_id == 2);
    CHECK_FALSE(t.column("id")->nullable);  // a primary key column is never null
    CHECK(t.column("name")->nullable);

    // A row written before the index exists is backfilled into it.
    RowWriter w{t.layout};
    REQUIRE(w.set_i64(0, 7));
    REQUIRE(w.set_bytes(1, bytes("bob")));
    Bytes row;
    REQUIRE(w.finish(row));
    Bytes pk;
    REQUIRE(encode_pk_from_row(t, row, pk));
    db.put({}, pk, row);

    const auto& ix = cat.create_index(db, "t_by_name", "T", {"name"});
    CHECK(ix.id == 3);
    CHECK(cat.version() == 2);
    version = cat.version();

    Bytes ik;
    key_put_prefix(ik, kNsIndex, ix.id);
    key_put_bytes(ik, bytes("bob"), {PhysType::Bytes, true});
    key_put_i64(ik, 7, {PhysType::I64});
    Bytes out;
    CHECK(db.get({}, ik, out));

    CHECK_THROWS_AS(create_t(cat, db), std::invalid_argument);
    CHECK_THROWS_AS(cat.create_index(db, "t", "t", {"qty"}), std::invalid_argument);
    CHECK_THROWS_AS(cat.create_index(db, "i2", "t", {"nope"}), std::invalid_argument);
    CHECK(cat.version() == version);
    db.close();
  }
  auto db = bytecask::DB::open(dir.path());
  auto cat = Catalog::load(db);
  CHECK(cat.version() == version);
  const auto* t = cat.table("t");
  REQUIRE(t != nullptr);
  CHECK(t->version == 2);
  REQUIRE(t->columns.size() == 3);
  CHECK(t->column("qty")->id == 2);
  CHECK(t->next_col_id == 3);
  REQUIRE(t->index("t_by_name") != nullptr);
  CHECK(t->index("t_by_name")->col_ids == std::vector<std::uint32_t>{1});

  const auto* kl = cat.key_layout(3);
  REQUIRE(kl != nullptr);
  CHECK(kl->ns == kNsIndex);
  CHECK(kl->index_parts == 1);
  REQUIRE(kl->parts.size() == 2);
  CHECK(kl->parts[0].type == PhysType::Bytes);
  CHECK(kl->parts[0].nullable);
  CHECK(kl->parts[1].type == PhysType::I64);
  CHECK(cat.key_layout(1)->ns == kNsRow);
  CHECK(cat.row_layout(2) == &t->layout);

  // The next id continues from the persisted counter.
  const auto& u = cat.create_table(db, "u", {{"k", PhysType::I64, false}}, {"k"});
  CHECK(u.id == 4);
}

TEST_CASE("catalog: a record of the prototype's format 1 is refused, not misread", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_format"};
  auto db = bytecask::DB::open(dir.path());
  // 01 02 <id:4>: a table record whose format field (u16, little-endian) is 1.
  Bytes key{std::byte{kNsCatalog}, std::byte{0x02}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
  db.put({}, key, Bytes{std::byte{1}, std::byte{0}, std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}});
  try {
    (void)Catalog::load(db);
    FAIL("a format-1 record must be refused");
  } catch (const std::runtime_error& e) {
    CHECK(std::string{e.what()}.find("earlier SQL-layer build") != std::string::npos);
  }
}

TEST_CASE("catalog: a record whose row layout breaks the format is refused at load", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_layout"};
  Bytes key;
  {
    auto db = bytecask::DB::open(dir.path());
    auto cat = Catalog::load(db);
    const auto& t = create_t(cat, db);
    key = {std::byte{kNsCatalog}, std::byte{0x02}, std::byte{0}, std::byte{0}, std::byte{0},
           static_cast<std::byte>(t.id)};
    Bytes rec;
    REQUIRE(db.get({}, key, rec));
    // format u16, id u32, name (u16 length + "t"), version u32, layout id u32,
    // generation u16: bitmap_bytes sits at byte 19.
    rec[19] = std::byte{7};
    rec[20] = std::byte{0};
    db.put({}, key, rec);
    db.close();
  }
  auto db = bytecask::DB::open(dir.path());
  CHECK_THROWS_AS(Catalog::load(db), std::runtime_error);
}

TEST_CASE("catalog: the constants block a plan needs", "[sql][catalog]") {
  sql_test::TempDir dir{"catalog_consts"};
  auto db = bytecask::DB::open(dir.path());
  auto cat = Catalog::load(db);
  const auto& t = create_t(cat, db);
  (void)cat.create_index(db, "t_by_name", "t", {"name"});
  auto cs = cat.constants(*cat.table("t"));
  CHECK(constant(cs, "TBL_T") == 1);
  CHECK(constant(cs, "LAY_T") == 2);
  CHECK(constant(cs, "IDX_T_BY_NAME") == 3);
  CHECK(constant(cs, "T_ID_OFF") == t.layout.find(0)->pos);
  CHECK(constant(cs, "T_QTY_OFF") == t.layout.find(2)->pos);
  CHECK(constant(cs, "T_NAME_SLOT") == 0);
  CHECK(constant(cs, "T_NAME_COL") == 1);
}
