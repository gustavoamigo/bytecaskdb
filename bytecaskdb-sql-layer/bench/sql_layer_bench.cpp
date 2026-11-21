// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sql_layer_bench — micro-benchmarks of the plan API's fixed costs, on the
// TPROC-C schema: what bc_ctx_run's verification of its reference, the
// compatibility check and the map from column ids to positions, adds to an
// execution (a point read's reference and a New-Order-sized one), what one
// column decode costs, what a nullable text column's null test and read
// cost, and what bc_update costs when no indexed column changes; and the
// bytes a row of each table takes as a record. Then the SERIALIZABLE rows
// (docs/transaction_design.md, *Stages of serializable isolation*, stage
// 1): a transaction's time at Serializable and at Snapshot, and the p99 of
// snapshot-isolation writers alone and alongside Serializable writers that
// scanned index ranges, which the engine checks before its writer section
// unless the index was written since (*Read-guard cost at commit and the
// writer critical section*), with Snapshot range writers as the control. Not part
// of CI. The published
// numbers were taken in releasedbg (-O3 -march=native, no LTO):
//   xmake f -m releasedbg --sql_layer=y && xmake build sql_layer_bench && xmake run sql_layer_bench
// release adds LTO, whose link fails where llvm-ar is older than clang (it
// cannot read the newer bitcode); releasedbg avoids it.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "bytecaskdb-sql-layer/algebra/bc_plan.h"
#include "include/bytecask.hpp"

import bytecask.sql.runtime;
import bytecask.sql.parser;
import bytecask.sql.codegen;
import bytecask.sql.api;

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

auto make_ref(const Catalog& cat, const RefSpec& s) -> std::unique_ptr<Ref> {
  auto r = std::make_unique<Ref>();
  for (const auto& name : s.read) r->read.push_back(cat.table({kPublicSchema, name})->id);
  for (const auto& name : s.written) r->written.push_back(cat.table({kPublicSchema, name})->id);
  for (const auto& [tn, cn] : s.cols) {
    const auto* t = cat.table({kPublicSchema, tn});
    const auto* c = t->column(cn);
    r->cols.push_back({t->id, c->id, static_cast<std::uint8_t>(c->type)});
  }
  r->result = result_type_codes(s.result);
  r->ref = {cat.version(),
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
  for (const auto& c : cat.table({kPublicSchema, std::string{table}})->columns) out.emplace_back(std::string{table}, c.name);
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

// Reads customer (1, 1, 1) and, 64 times, tests one of its nullable VARCHAR
// columns for null and reads its bytes when present, as generated plans do:
// c_first and c_last are set, c_middle and c_street_1 are NULL. The
// per-column cost of a nullable text column's null test and read.
std::array<std::uint32_t, 4> g_texts{};
void text_64(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, g_customer, 0, 3), &found);
  for (int i = 0; i < 64; ++i) {
    auto col = g_texts[static_cast<std::size_t>(i) % g_texts.size()];
    if (!bc_col_null(cx, row, col)) (void)bc_col_bytes(cx, row, col);
  }
  bc_emit_count(cx, found ? 1 : 0);
}
void text_0(bc_ctx* cx) {
  bool found = false;
  bc_view row = bc_get(cx, bc_key_params(cx, g_customer, 0, 3), &found);
  (void)row;
  bc_emit_count(cx, found ? 1 : 0);
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

// The bytes one row of each TPROC-C table takes in the engine, as the codec
// lays it out: every column present, integers 1, a VARCHAR(n) n bytes long
// (the most it holds, so the strings are the same in every format). A
// record is its key, its value and the engine's 19 bytes of framing; an
// index entry is its key and the framing. New-Order is the issue #780
// count: one district, orders and new_order row, an orders_i2 entry, ten
// order_line and ten stock rows.
void print_record_bytes(const Catalog& cat) {
  constexpr std::size_t kFraming = 19;
  std::printf("record bytes (key + value + %zu framing), every column present:\n", kFraming);
  auto record = [&](std::string_view name) {
    const auto& t = *cat.table({kPublicSchema, std::string{name}});
    RowWriter rw{t.layout};
    for (const auto& c : t.columns) {
      switch (c.type) {
        case PhysType::I64: (void)rw.set_i64(c.id, 1); break;
        case PhysType::F64: (void)rw.set_f64(c.id, 1.0); break;
        case PhysType::Bool: (void)rw.set_bool(c.id, true); break;
        case PhysType::Bytes: {
          std::string s(c.sql.length, 'x');
          (void)rw.set_bytes(c.id, std::as_bytes(std::span{s}));
          break;
        }
        case PhysType::Num: break;  // no column is a NUMERIC of no precision
      }
    }
    Bytes row, key;
    (void)rw.finish(row);
    (void)encode_pk_from_row(t, row, key);
    auto total = key.size() + row.size() + kFraming;
    std::printf("  %-10s key %3zu  value %4zu  record %4zu", std::string{name}.c_str(), key.size(), row.size(), total);
    std::size_t entries = 0;
    for (const auto& ix : t.indexes) {
      Bytes ik;
      (void)encode_index_key_from_row(t, ix, row, ik);
      std::printf("  %s %zu", ix.name.c_str(), ik.size() + kFraming);
      entries += ik.size() + kFraming;
    }
    std::printf("\n");
    return std::pair{total, entries};
  };
  std::size_t new_order = 0;
  for (auto name : {"warehouse", "district", "customer", "history", "new_order", "orders", "order_line", "item",
                    "stock"}) {
    auto [rec, ix] = record(name);
    std::string_view n{name};
    if (n == "district" || n == "new_order") new_order += rec;
    if (n == "orders") new_order += rec + ix;
    if (n == "order_line" || n == "stock") new_order += 10 * rec;
  }
  std::printf("  New-Order writes (district, orders + orders_i2, new_order, 10 order_line, 10 stock): %zu bytes\n",
              new_order);
}

}  // namespace

namespace {

auto percentile(std::vector<double> v, double p) -> double {
  if (v.empty()) return 0;
  std::ranges::sort(v);
  return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

// The SERIALIZABLE rows. kv holds 1M rows, g = k, indexed; w is what every
// writer updates, a row each, so no two of them conflict.
void serializable_rows() {
  using Clock = std::chrono::steady_clock;
  auto dir = std::filesystem::temp_directory_path() / ("sql_layer_bench_ser_" + std::to_string(std::random_device{}()));
  std::filesystem::remove_all(dir);
  {
    auto db = Database::open(dir);
    (void)db->execute("CREATE TABLE kv (k BIGINT PRIMARY KEY, g BIGINT NOT NULL, v BIGINT NOT NULL)");
    (void)db->execute("CREATE INDEX kv_g ON kv (g)");
    (void)db->execute("CREATE TABLE w (k BIGINT PRIMARY KEY, v BIGINT NOT NULL)");
    constexpr std::int64_t kRows = 1'000'000;
    {
      const auto& ins = db->prepare("INSERT INTO kv VALUES ($1, $1, 0)");
      for (std::int64_t k = 0; k < kRows;) {
        auto t = db->begin({.max_bytes = std::uint64_t{1} << 30});
        for (auto end = std::min(kRows, k + 20'000); k < end; ++k) (void)t.execute(ins, {k});
        (void)t.commit();
      }
      const auto& insw = db->prepare("INSERT INTO w VALUES ($1, 0)");
      for (std::int64_t k = 0; k < 100'000; ++k) (void)db->execute(insw, {k});
    }
    std::mt19937_64 rng{7};
    const auto& point = db->prepare("SELECT v FROM kv WHERE k = $1");
    const auto& bump = db->prepare("UPDATE w SET v = v + 1 WHERE k = $1");
    const auto& fetch = db->prepare("SELECT v FROM kv WHERE g BETWEEN $1 AND $2");
    const auto& covering = db->prepare("SELECT g FROM kv WHERE g BETWEEN $1 AND $2");
    const auto& full = db->prepare("SELECT max(v) FROM kv");
    auto key = [&](std::int64_t n) { return static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(n)); };
    auto us_per_txn = [&](Isolation level, int iters, auto&& body) {
      std::vector<double> runs;
      for (int r = 0; r < 5; ++r) {
        auto t0 = Clock::now();
        for (int i = 0; i < iters; ++i) {
          auto t = db->begin({.max_bytes = std::uint64_t{1} << 30, .isolation = level});
          body(t);
          (void)t.commit();
        }
        runs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / iters);
      }
      return percentile(runs, 0.5);
    };
    std::printf("\nSERIALIZABLE transactions, us per transaction (Snapshot / Serializable):\n");
    auto row = [&](const char* name, int iters, auto&& body) {
      auto snap = us_per_txn(Isolation::Snapshot, iters, body);
      auto ser = us_per_txn(Isolation::Serializable, iters, body);
      std::printf("  %-48s %10.1f %10.1f\n", name, snap, ser);
    };
    row("ten point reads and one write", 2000, [&](Transaction& t) {
      for (int k = 0; k < 10; ++k) (void)t.execute(point, {key(kRows)});
      (void)t.execute(bump, {key(100'000)});
    });
    for (std::int64_t n : {100, 10'000}) {
      const auto name = "index range of " + std::to_string(n) + " rows fetched, one write";
      row(name.c_str(), n == 100 ? 1000 : 20, [&](Transaction& t) {
        const auto lo = key(kRows - n);
        (void)t.execute(fetch, {lo, lo + n - 1});
        (void)t.execute(bump, {key(100'000)});
      });
    }
    row("read-only full scan of 1M rows", 3, [&](Transaction& t) { (void)t.execute(full, {}); });

    // Writer p99: four snapshot-isolation writers, each an autocommit UPDATE
    // of a row of its own, alone and alongside two Serializable writers
    // that read an index range of n keys (covering: one range guard) and
    // write a row of their own. The same two writers at Snapshot, which
    // submit no range guard, are the control: what is left of the effect
    // there is the two extra threads' work, not the commit's check.
    std::printf("\nwriter latency, snapshot-isolation autocommit UPDATE, 4 threads, us (p50 / p99 / p99.9, txns):\n");
    auto scenario = [&](const char* name, std::int64_t n, Isolation level = Isolation::Serializable) {
      std::atomic<bool> stop{false};
      std::vector<std::vector<double>> lat(4);
      std::atomic<std::uint64_t> ser_done{0}, ser_failed{0};
      {
        std::vector<std::jthread> threads;
        for (std::size_t t = 0; t < 4; ++t) {
          threads.emplace_back([&, t] {
            std::mt19937_64 r{t};
            while (!stop.load()) {
              auto t0 = Clock::now();
              (void)db->execute(bump, {static_cast<std::int64_t>(t * 1000 + r() % 1000)});
              lat[t].push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
            }
          });
        }
        for (std::size_t t = 0; n > 0 && t < 2; ++t) {
          threads.emplace_back([&, t] {
            std::mt19937_64 r{100 + t};
            while (!stop.load()) {
              try {
                auto x = db->begin({.max_bytes = std::uint64_t{1} << 30, .isolation = level});
                const auto lo = n >= kRows ? 0 : static_cast<std::int64_t>(r() % static_cast<std::uint64_t>(kRows - n));
                (void)x.execute(covering, {lo, lo + n - 1});
                (void)x.execute(bump, {static_cast<std::int64_t>(50'000 + t * 1000 + r() % 1000)});
                (void)x.commit();
                ser_done.fetch_add(1);
              } catch (const SerializationFailure&) {
                ser_failed.fetch_add(1);
              }
            }
          });
        }
        std::this_thread::sleep_for(std::chrono::seconds{6});
        stop.store(true);
      }
      std::vector<double> all;
      for (auto& l : lat) all.insert(all.end(), l.begin(), l.end());
      std::printf("  %-44s %8.1f %8.1f %9.1f %8zu", name, percentile(all, 0.5), percentile(all, 0.99),
                  percentile(all, 0.999), all.size());
      if (n > 0) std::printf("   range writers: %llu committed, %llu 40001", static_cast<unsigned long long>(ser_done.load()),
                             static_cast<unsigned long long>(ser_failed.load()));
      std::printf("\n");
    };
    scenario("alone", 0);
    scenario("with 2 Serializable writers, ranges of 100", 100);
    scenario("with 2 Serializable writers, ranges of 10000", 10'000);
    scenario("with 2 Serializable writers, ranges of 1000000", kRows);
    scenario("with 2 Snapshot writers, ranges of 10000", 10'000, Isolation::Snapshot);
    scenario("alone, again", 0);
    db->close();
  }
  std::filesystem::remove_all(dir);
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
    print_record_bytes(*cat);
    // One warehouse row, written straight to the engine.
    const auto& w = *cat->table({kPublicSchema, "warehouse"});
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
    const auto& cust = *cat->table({kPublicSchema, "customer"});
    g_customer = cust.id;
    g_balance = cust.column("c_balance")->id;
    g_texts = {cust.column("c_first")->id, cust.column("c_middle")->id, cust.column("c_last")->id,
               cust.column("c_street_1")->id};
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
    auto* cx = bc_ctx_new(&bdb, &bcat);
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
    // Every run has a reference and verifies it: the baseline is an empty
    // one, so the lines below are what a reference's contents add.
    auto empty = make_ref(*cat, {});
    auto base = runs(&empty->ref);
    std::printf("bc_ctx_run of an empty plan, empty schema ref: %7.1f ns\n", base);
    for (const auto& [name, spec] : {std::pair{"point read", &point}, std::pair{"New-Order", &new_order}}) {
      auto ref = make_ref(*cat, *spec);
      std::printf("%-10s (%2zu cols, %2zu objects): verify and map ids, per run %7.1f ns\n", name, spec->cols.size(),
                  spec->read.size() + spec->written.size(), runs(&ref->ref) - base);
    }
    auto wref = make_ref(*cat, {{"warehouse"}, {}, {{"warehouse", "w_tax"}}, {}});
    auto run_with = [&](const bc_params& p, bc_plan_fn plan, const bc_schema_ref* ref) {
      bc_ctx_begin(cx, &p, {});
      bc_ctx_run(cx, plan, ref);
      if (bc_error(cx) != BC_OK) std::printf("error: %s\n", bc_error_message(cx));
      bc_ctx_reset(cx);
    };
    auto run = [&](bc_plan_fn plan, const bc_schema_ref* ref) { run_with(ps, plan, ref); };
    auto d0 = ns_per_iter([&] { run(decode_0, &wref->ref); }, kIters);
    auto d64 = ns_per_iter([&] { run(decode_64, &wref->ref); }, kIters);
    std::printf("bc_col_i64 + bc_add, by column id:         %8.1f ns per call\n", (d64 - d0) / 64);
    auto tref = make_ref(*cat, {{"customer"},
                                {},
                                {{"customer", "c_first"},
                                 {"customer", "c_middle"},
                                 {"customer", "c_last"},
                                 {"customer", "c_street_1"}},
                                {}});
    bc_params ps3{{std::int64_t{1}, std::int64_t{1}, std::int64_t{1}}};
    auto t0 = ns_per_iter([&] { run_with(ps3, text_0, &tref->ref); }, kIters);
    auto t64 = ns_per_iter([&] { run_with(ps3, text_64, &tref->ref); }, kIters);
    std::printf("bc_col_null + bc_col_bytes, nullable VARCHAR:  %8.1f ns per column\n", (t64 - t0) / 64);
    auto pref = make_ref(*cat, {{"customer"}, {"customer"}, {{"customer", "c_balance"}}, {}});
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
  serializable_rows();
  return 0;
}
