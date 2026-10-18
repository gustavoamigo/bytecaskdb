// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <stdexcept>

#include "bytecaskdb-sql-layer/catalog/catalog.hpp"
#include "test_util.hpp"

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
    key_put_bytes(ik, bytes("bob"), true);
    key_put_i64(ik, 7, false);
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

  auto two = cat.result_layout({PhysType::I64, PhysType::I64});
  auto one = cat.result_layout({PhysType::I64});
  CHECK(two == Catalog::kResultLayoutBase);
  CHECK(one == Catalog::kResultLayoutBase + 1);
  CHECK(cat.result_layout({PhysType::I64, PhysType::I64}) == two);
  CHECK(cat.result_types(one)->size() == 1);
}
