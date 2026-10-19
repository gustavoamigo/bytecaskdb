// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sql_layer_bench — micro-benchmarks of the plan API's fixed costs, on the
// TPROC-C schema: what bc_ctx_run's compatibility check adds to an execution
// (a point read's reference and a New-Order-sized one), what verifying a
// reference and mapping its column ids to positions costs when it is not
// cached, what one column decode costs, and what bc_update costs when no
// indexed column changes. Not part of CI. The published
// numbers were taken in releasedbg (-O3 -march=native, no LTO):
//   xmake f -m releasedbg && xmake build sql_layer_bench && xmake run sql_layer_bench
// release adds LTO, whose link fails where llvm-ar is older than clang (it
// cannot read the newer bitcode); releasedbg avoids it.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.runtime;
import bytecask.sql.parser;
import bytecask.sql.codegen;

using namespace bytecask::sql;

namespace {

constexpr std::array<std::string_view, 11> kSchema{
    "CREATE TABLE warehouse (w_id INTEGER NOT NULL, w_ytd NUMERIC(12, 2), w_tax NUMERIC(4, 4), "
    "w_name VARCHAR(10), w_street_1 VARCHAR(20), w_street_2 VARCHAR(20), w_city VARCHAR(20), "
    "w_state VARCHAR(2), w_zip VARCHAR(9), PRIMARY KEY (w_id))",
    "CREATE TABLE district (d_id SMALLINT NOT NULL, d_w_id INTEGER NOT NULL, d_ytd NUMERIC(12, 2), "
    "d_tax NUMERIC(4, 4), d_next_o_id INTEGER, d_name VARCHAR(10), d_street_1 VARCHAR(20), "
    "d_street_2 VARCHAR(20), d_city VARCHAR(20), d_state VARCHAR(2), d_zip VARCHAR(9), "
    "PRIMARY KEY (d_w_id, d_id))",
    "CREATE TABLE customer (c_id INTEGER NOT NULL, c_d_id SMALLINT NOT NULL, c_w_id INTEGER NOT NULL, "
    "c_first VARCHAR(16), c_middle VARCHAR(2), c_last VARCHAR(16), c_street_1 VARCHAR(20), "
    "c_street_2 VARCHAR(20), c_city VARCHAR(20), c_state VARCHAR(2), c_zip VARCHAR(9), c_phone VARCHAR(16), "
    "c_since TIMESTAMP, c_credit VARCHAR(2), c_credit_lim NUMERIC(12, 2), c_discount NUMERIC(4, 4), "
    "c_balance NUMERIC(12, 2), c_ytd_payment NUMERIC(12, 2), c_payment_cnt INTEGER, c_delivery_cnt INTEGER, "
    "c_data VARCHAR(500), PRIMARY KEY (c_w_id, c_d_id, c_id))",
    "CREATE UNIQUE INDEX customer_i2 ON customer (c_w_id, c_d_id, c_last, c_first, c_id)",
    "CREATE TABLE history (h_id BIGINT NOT NULL, h_c_id INTEGER, h_c_d_id SMALLINT, h_c_w_id INTEGER, "
    "h_d_id SMALLINT, h_w_id INTEGER, h_date TIMESTAMP, h_amount NUMERIC(6, 2), h_data VARCHAR(24), "
    "PRIMARY KEY (h_id))",
    "CREATE TABLE new_order (no_w_id INTEGER NOT NULL, no_d_id SMALLINT NOT NULL, no_o_id INTEGER NOT NULL, "
    "PRIMARY KEY (no_w_id, no_d_id, no_o_id))",
    "CREATE TABLE orders (o_id INTEGER NOT NULL, o_w_id INTEGER NOT NULL, o_d_id SMALLINT NOT NULL, "
    "o_c_id INTEGER, o_carrier_id SMALLINT, o_ol_cnt SMALLINT, o_all_local SMALLINT, o_entry_d TIMESTAMP, "
    "PRIMARY KEY (o_w_id, o_d_id, o_id))",
    "CREATE UNIQUE INDEX orders_i2 ON orders (o_w_id, o_d_id, o_c_id, o_id)",
    "CREATE TABLE order_line (ol_w_id INTEGER NOT NULL, ol_d_id SMALLINT NOT NULL, ol_o_id INTEGER NOT NULL, "
    "ol_number SMALLINT NOT NULL, ol_i_id INTEGER, ol_delivery_d TIMESTAMP, ol_amount NUMERIC(6, 2), "
    "ol_supply_w_id INTEGER, ol_quantity SMALLINT, ol_dist_info VARCHAR(24), "
    "PRIMARY KEY (ol_w_id, ol_d_id, ol_o_id, ol_number))",
    "CREATE TABLE item (i_id INTEGER NOT NULL, i_im_id INTEGER, i_name VARCHAR(24), i_price NUMERIC(5, 2), "
    "i_data VARCHAR(50), PRIMARY KEY (i_id))",
    "CREATE TABLE stock (s_i_id INTEGER NOT NULL, s_w_id INTEGER NOT NULL, s_quantity SMALLINT, "
    "s_dist_01 VARCHAR(24), s_dist_02 VARCHAR(24), s_dist_03 VARCHAR(24), s_dist_04 VARCHAR(24), "
    "s_dist_05 VARCHAR(24), s_dist_06 VARCHAR(24), s_dist_07 VARCHAR(24), s_dist_08 VARCHAR(24), "
    "s_dist_09 VARCHAR(24), s_dist_10 VARCHAR(24), s_ytd INTEGER, s_order_cnt SMALLINT, s_remote_cnt SMALLINT, "
    "s_data VARCHAR(50), PRIMARY KEY (s_i_id, s_w_id))",
};

// What a plan's bc_schema_ref lists, by name.
struct RefSpec {
  std::vector<std::string> read, written;
  std::vector<std::pair<std::string, std::string>> cols;  // table, column
  std::vector<PhysType> result;
};

// A bc_schema_ref and the arrays it points into.
struct Ref {
  std::vector<std::uint32_t> read, written;
  std::vector<bc_col_ref> cols;
  std::vector<std::uint8_t> result;
  bc_schema_ref ref{};
};

auto n32(std::size_t n) -> std::uint32_t { return static_cast<std::uint32_t>(n); }

auto make_ref(const Catalog& cat, const RefSpec& s, std::uint64_t id) -> std::unique_ptr<Ref> {
  auto r = std::make_unique<Ref>();
  for (const auto& name : s.read) r->read.push_back(cat.table(name)->id);
  for (const auto& name : s.written) r->written.push_back(cat.table(name)->id);
  for (const auto& [tn, cn] : s.cols) {
    const auto* t = cat.table(tn);
    const auto* c = t->column(cn);
    r->cols.push_back({t->id, c->id, static_cast<std::uint8_t>(c->type)});
  }
  r->result = result_type_codes(s.result);
  r->ref = {id,
            cat.version(),
            r->read.data(),
            n32(r->read.size()),
            r->written.data(),
            n32(r->written.size()),
            r->cols.data(),
            n32(r->cols.size()),
            r->result.data(),
            n32(r->result.size())};
  return r;
}

auto all_columns(const Catalog& cat, std::string_view table) -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& c : cat.table(table)->columns) out.emplace_back(std::string{table}, c.name);
  return out;
}

void noop(bc_ctx*) {}

// Reads warehouse 1 and decodes w_tax 64 times: the per-access cost of a
// column decoder, by id.
std::uint32_t g_wtax = 0;
std::uint32_t g_warehouse = 0;
void decode_64(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, g_warehouse, 0, 1), &found);
  std::int64_t sum = 0;
  for (int i = 0; i < 64; ++i) sum = bc_add(cx, sum, bc_col_i64(cx, row, g_wtax));
  bc_emit_count(cx, sum);
}
void decode_0(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, g_warehouse, 0, 1), &found);
  (void)row;
  bc_emit_count(cx, found ? 1 : 0);
}

// Payment's customer update: customer (1, 1, 1) read by primary key and its
// c_balance set. payment_update writes it with bc_update; payment_built stops
// at the built row, so the difference is bc_update's cost on a table whose
// one index, customer_i2, has no column the update changes.
std::uint32_t g_customer = 0;
std::uint32_t g_balance = 0;
auto payment_row(bc_ctx* cx, bc_view* old) -> bc_view {
  bool found = false;
  *old = bc_get(cx, bc_key_params(cx, g_customer, 0, 3), &found);
  bc_row r = bc_row_from(cx, g_customer, *old);
  bc_row_set_i64(cx, r, g_balance, 5);
  return bc_row_end(cx, r);
}
void payment_built(bc_ctx* cx) {
  bc_view old{};
  (void)payment_row(cx, &old);
}
void payment_update(bc_ctx* cx) {
  bc_view old{};
  bc_view row = payment_row(cx, &old);
  bc_update(cx, g_customer, old, row);
}

template <typename F>
auto ns_per_iter(F&& f, int iters) -> double {
  std::vector<double> runs;
  for (int r = 0; r < 7; ++r) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) f();
    auto t1 = std::chrono::steady_clock::now();
    runs.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count() / iters);
  }
  std::ranges::sort(runs);
  return runs[runs.size() / 2];
}

// What b costs over a: the median, over 31 rounds, of the difference
// between back-to-back timings of the two, so drift between rounds cancels.
template <typename A, typename B>
auto ns_over(A&& a, B&& b, int iters) -> double {
  auto time = [&](auto& f) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) f();
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / iters;
  };
  std::vector<double> diffs;
  for (int r = 0; r < 31; ++r) {
    auto ta = time(a);
    diffs.push_back(time(b) - ta);
  }
  std::ranges::sort(diffs);
  return diffs[diffs.size() / 2];
}

}  // namespace

int main() {
  auto dir = std::filesystem::temp_directory_path() / ("sql_layer_bench_" + std::to_string(std::random_device{}()));
  std::filesystem::remove_all(dir);
  {
    auto db = bytecask::DB::open(dir);
    LiveCatalog live{db};
    for (auto sql : kSchema) {
      auto b = bound::bind(ast::parse(sql), *live.current());
      (void)live.apply(std::get<Ddl>(b.stmt));
    }
    auto cat = live.current();
    // One warehouse row, written straight to the engine.
    const auto& w = *cat->table("warehouse");
    g_warehouse = w.id;
    g_wtax = w.column("w_tax")->id;
    {
      RowWriter rw{w.layout};
      (void)rw.set_i64(w.column("w_id")->id, 1);
      (void)rw.set_i64(g_wtax, 1234);
      Bytes row, key;
      (void)rw.finish(row);
      (void)encode_pk_from_row(w, row, key);
      db.put({}, key, row);
    }
    // One customer row, likewise.
    const auto& cust = *cat->table("customer");
    g_customer = cust.id;
    g_balance = cust.column("c_balance")->id;
    {
      RowWriter rw{cust.layout};
      for (const auto* name : {"c_w_id", "c_d_id", "c_id"}) (void)rw.set_i64(cust.column(name)->id, 1);
      (void)rw.set_bytes(cust.column("c_first")->id, std::as_bytes(std::span{std::string_view{"first"}}));
      (void)rw.set_bytes(cust.column("c_last")->id, std::as_bytes(std::span{std::string_view{"BARBARBAR"}}));
      (void)rw.set_i64(g_balance, 0);
      Bytes row, key;
      (void)rw.finish(row);
      (void)encode_pk_from_row(cust, row, key);
      db.put({}, key, row);
    }

    bc_db bdb{&db};
    bc_catalog bcat{&live};
    auto* cx = bc_ctx_new(&bdb, &bcat, 1u << 20);
    bc_params ps{{std::int64_t{1}}};

    RefSpec point{{"customer"}, {}, {{"customer", "c_id"}, {"customer", "c_balance"}}, {PhysType::I64, PhysType::I64}};
    RefSpec new_order{{"warehouse", "district", "customer", "item", "stock"},
                      {"district", "orders", "new_order", "order_line", "stock"},
                      {{"warehouse", "w_tax"},
                       {"district", "d_tax"},
                       {"district", "d_next_o_id"},
                       {"customer", "c_discount"},
                       {"customer", "c_last"},
                       {"customer", "c_credit"},
                       {"item", "i_price"},
                       {"item", "i_name"},
                       {"item", "i_data"},
                       {"stock", "s_quantity"},
                       {"stock", "s_dist_01"},
                       {"stock", "s_ytd"},
                       {"stock", "s_order_cnt"},
                       {"stock", "s_remote_cnt"},
                       {"stock", "s_data"}},
                      {PhysType::I64, PhysType::Bytes, PhysType::Bytes, PhysType::I64, PhysType::I64}};
    for (const auto& c : all_columns(*cat, "orders")) new_order.cols.push_back(c);
    for (const auto& c : all_columns(*cat, "new_order")) new_order.cols.push_back(c);
    for (const auto& c : all_columns(*cat, "order_line")) new_order.cols.push_back(c);

    constexpr int kIters = 200000;
    // bc_ctx_run alone, repeated under one pinned snapshot: the check and
    // the call, with nothing else in the loop.
    auto runs = [&](const bc_schema_ref* ref) {
      bc_ctx_begin(cx, &ps, {});
      auto t = ns_per_iter([&] { bc_ctx_run(cx, noop, ref); }, kIters);
      if (bc_error(cx) != BC_OK) std::printf("error: %s\n", bc_error_message(cx));
      bc_ctx_reset(cx);
      return t;
    };
    auto base = runs(nullptr);
    std::printf("bc_ctx_run of an empty plan, no schema ref: %7.1f ns\n", base);
    for (const auto& [name, spec] : {std::pair{"point read", &point}, std::pair{"New-Order", &new_order}}) {
      auto cached = make_ref(*cat, *spec, bc_ref_id());
      auto uncached = make_ref(*cat, *spec, 0);  // id 0: verified on every run
      auto hit = runs(&cached->ref) - base;
      auto miss = runs(&uncached->ref) - base;
      std::printf("%-10s (%2zu cols, %2zu objects): check, cached %7.1f ns; verify and map ids %7.1f ns\n", name,
                  spec->cols.size(), spec->read.size() + spec->written.size(), hit, miss);
    }
    // 50 New-Order-sized references run in turn, as a workload of many
    // prepared statements does: each must stay cached between its runs.
    {
      std::vector<std::unique_ptr<Ref>> many;
      for (int i = 0; i < 50; ++i) many.push_back(make_ref(*cat, new_order, bc_ref_id()));
      std::size_t at = 0;
      bc_ctx_begin(cx, &ps, {});
      for (const auto& r : many) bc_ctx_run(cx, noop, &r->ref);  // the first verification of each
      auto cycled = ns_per_iter(
          [&] {
            bc_ctx_run(cx, noop, &many[at]->ref);
            at = (at + 1) % many.size();
          },
          kIters);
      if (bc_error(cx) != BC_OK) std::printf("error: %s\n", bc_error_message(cx));
      bc_ctx_reset(cx);
      std::printf("50 New-Order refs in turn: check per run %7.1f ns\n", cycled - base);
    }
    // 1,500 dead references fill the cache, as recompiles leave it, then two
    // live ones run in turn: CLOCK evicts the dead, and both hit.
    {
      std::vector<std::unique_ptr<Ref>> dead;
      for (int i = 0; i < 1500; ++i) dead.push_back(make_ref(*cat, new_order, bc_ref_id()));
      auto a = make_ref(*cat, new_order, bc_ref_id());
      auto b = make_ref(*cat, new_order, bc_ref_id());
      bc_ctx_begin(cx, &ps, {});
      for (const auto& r : dead) bc_ctx_run(cx, noop, &r->ref);
      auto turn = false;
      auto alternating = ns_per_iter(
          [&] {
            bc_ctx_run(cx, noop, turn ? &a->ref : &b->ref);
            turn = !turn;
          },
          kIters);
      if (bc_error(cx) != BC_OK) std::printf("error: %s\n", bc_error_message(cx));
      bc_ctx_reset(cx);
      std::printf("1,500 dead refs, then 2 live ones in turn: check per run %7.1f ns\n", alternating - base);
    }
    auto wref = make_ref(*cat, {{"warehouse"}, {}, {{"warehouse", "w_tax"}}, {}}, bc_ref_id());
    auto run = [&](bc_plan_fn plan, const bc_schema_ref* ref) {
      bc_ctx_begin(cx, &ps, {});
      bc_ctx_run(cx, plan, ref);
      if (bc_error(cx) != BC_OK) std::printf("error: %s\n", bc_error_message(cx));
      bc_ctx_reset(cx);
    };
    auto d0 = ns_per_iter([&] { run(decode_0, &wref->ref); }, kIters);
    auto d64 = ns_per_iter([&] { run(decode_64, &wref->ref); }, kIters);
    std::printf("bc_col_i64 + bc_add, by column id:         %8.1f ns per call\n", (d64 - d0) / 64);
    auto pref = make_ref(*cat, {{"customer"}, {"customer"}, {{"customer", "c_balance"}}, {}}, bc_ref_id());
    bc_params ps3{{std::int64_t{1}, std::int64_t{1}, std::int64_t{1}}};
    auto run3 = [&](bc_plan_fn plan) {
      bc_ctx_begin(cx, &ps3, {});
      bc_ctx_run(cx, plan, &pref->ref);
      if (bc_error(cx) != BC_OK) std::printf("error: %s\n", bc_error_message(cx));
      bc_ctx_reset(cx);
    };
    auto update = ns_over([&] { run3(payment_built); }, [&] { run3(payment_update); }, kIters / 20);
    std::printf("bc_update, Payment's customer (no index column changed): %7.1f ns\n", update);
    bc_ctx_free(cx);
    db.close();
  }
  std::filesystem::remove_all(dir);
  return 0;
}
