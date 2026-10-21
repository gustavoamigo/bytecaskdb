// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// tpcc.cppm — the MVP gate harness (docs/testing_design.md, *The MVP gate
// harness*): TPC-C's initial population, TPROC-C's transactions with the
// statements of docs/frontend_design.md, a closed-loop driver and its
// report, TPC-C's consistency conditions, and a list-append history through
// SQL for Elle. Everything goes through the embedded API. Shared by the
// sql_layer_tpcc binary and the smoke test in sql_layer_tests.

module;
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

export module bytecask.sql.tpcc;

import bytecask.sql.api;
import bytecask.sql.tests.tpcc;

export namespace bytecask::sql::tpcc {

// The population's sizes per warehouse and district. The defaults are the
// specification's (clause 4.3.3); only the smoke test shrinks them.
struct Scale {
  std::int64_t items{100'000};      // also the stock rows per warehouse
  std::int64_t customers{3'000};    // per district
  std::int64_t orders{3'000};       // per district, one per customer
  std::int64_t new_orders{900};     // the last orders of each district
};

inline constexpr std::int64_t kDistricts = 10;

struct LoadOptions {
  std::int64_t warehouses{1};
  Scale scale{};
  std::uint64_t seed{1};
  std::size_t rows_per_txn{2'000};  // statements per loading Transaction
};

// Creates the schema and harness_run, and loads TPC-C's initial population.
// Throws on any error.
void load(Database& db, const LoadOptions& opts);

enum class Kind : std::uint8_t { NewOrder, Payment, OrderStatus, Delivery, StockLevel };
inline constexpr std::size_t kKinds = 5;
[[nodiscard]] auto kind_name(Kind k) -> std::string_view;

// Latencies in log buckets, eight per octave from 1 µs: a run keeps no
// per-transaction list.
class Histogram {
public:
  void add(std::chrono::nanoseconds d);
  void merge(const Histogram& o);
  [[nodiscard]] auto count() const -> std::uint64_t { return count_; }
  // The upper bound of the bucket holding the q-th quantile, q in [0, 1].
  [[nodiscard]] auto quantile(double q) const -> std::chrono::microseconds;
  [[nodiscard]] auto max() const -> std::chrono::microseconds { return max_; }

private:
  static constexpr std::size_t kBuckets = 8 * 25;  // 1 µs to past 10 s
  std::array<std::uint64_t, kBuckets> buckets_{};
  std::uint64_t count_{0};
  std::chrono::microseconds max_{0};
};

struct KindStats {
  std::uint64_t completed{0};
  std::uint64_t retries{0};    // attempts that ended in SerializationFailure and were rerun
  std::uint64_t abandoned{0};  // still SerializationFailure on the last attempt
  Histogram latency;           // first attempt's start to the completing commit
};

struct PhaseReport {
  std::size_t threads{0};
  double seconds{0};
  std::uint64_t run{0};
  std::array<KindStats, kKinds> kinds{};
  std::int64_t fsyncs{0};
  std::int64_t group_writer_batches{0};
  [[nodiscard]] auto tpm() const -> double;
  [[nodiscard]] auto nopm() const -> std::optional<double>;  // nullopt while New-Order is not in the mix
};

struct RunOptions {
  std::vector<std::size_t> threads{1, 16, 48};
  std::chrono::milliseconds warmup{std::chrono::seconds{10}};
  std::chrono::milliseconds duration{std::chrono::seconds{60}};
  std::uint64_t seed{1};
  // Stop each thread after this many transactions, measured or not; 0 runs
  // for the duration. The smoke test sets it.
  std::uint64_t txns_per_thread{0};
};

inline constexpr int kMaxAttempts = 10;

// The transactions in the mix today. Each later front-end slice adds its own.
[[nodiscard]] auto mix() -> std::vector<Kind>;

// Runs each phase on the loaded database, handing each report to
// `on_phase` as the phase ends. Throws on any error but a
// SerializationFailure, which is retried and counted; the phase with the
// error ends at once.
void run(Database& db, const RunOptions& opts, const std::function<void(const PhaseReport&)>& on_phase);

// TPC-C's consistency conditions (clause 3.3.2) for the transactions in the
// mix: the conditions that do not hold, each described; empty when all do.
[[nodiscard]] auto consistency(Database& db) -> std::vector<std::string>;

[[nodiscard]] auto format_report(const PhaseReport& r) -> std::string;
[[nodiscard]] auto json_report(const PhaseReport& r) -> std::string;

// A list-append history through SQL, in the engine harness's JSON format
// (docs/isolation_checking_design.md), for scripts/sql_isolation_check.py.
enum class ElleConfig : std::uint8_t { Guarded, Unguarded, Blind };
struct ElleOptions {
  ElleConfig config{ElleConfig::Guarded};
  std::size_t threads{8};
  std::uint64_t txns{2'000};  // in all, across the threads
  std::uint64_t seed{1};
};
[[nodiscard]] auto elle_history(Database& db, const ElleOptions& opts) -> std::string;

}  // namespace bytecask::sql::tpcc

namespace bytecask::sql::tpcc {

namespace {

using Clock = std::chrono::steady_clock;
using Rng = std::mt19937_64;

auto text(std::string_view s) -> Value {
  auto b = std::as_bytes(std::span{s.data(), s.size()});
  return Bytes{b.begin(), b.end()};
}

auto i64(const Value& v) -> std::int64_t { return std::get<std::int64_t>(v); }

auto str(const Value& v) -> std::string {
  const auto& b = std::get<Bytes>(v);
  std::string s(b.size(), '\0');
  std::ranges::transform(b, s.begin(), [](std::byte c) { return static_cast<char>(c); });
  return s;
}

auto uniform(Rng& rng, std::int64_t lo, std::int64_t hi) -> std::int64_t {
  return std::uniform_int_distribution<std::int64_t>{lo, hi}(rng);
}

// NURand(A, x, y) of clause 2.1.6, with the run's C.
auto nurand(Rng& rng, std::int64_t a, std::int64_t x, std::int64_t y, std::int64_t c) -> std::int64_t {
  return (((uniform(rng, 0, a) | uniform(rng, x, y)) + c) % (y - x + 1)) + x;
}

// The C constants (clause 2.1.6.1): C_LAST at load and at run differ by a
// delta in [65, 119], not 96 or 112.
constexpr std::int64_t kCLastLoad = 157;
constexpr std::int64_t kCLastRun = 223;
constexpr std::int64_t kCId = 259;

constexpr std::array<std::string_view, 10> kSyllables{"BAR", "OUGHT", "ABLE", "PRI", "PRES",
                                                      "ESE", "ANTI",  "CALLY", "ATION", "EING"};

auto last_name(std::int64_t n) -> std::string {
  return std::string{kSyllables[static_cast<std::size_t>(n / 100)]} +
         std::string{kSyllables[static_cast<std::size_t>(n / 10 % 10)]} +
         std::string{kSyllables[static_cast<std::size_t>(n % 10)]};
}

// The last names a scale's customers take: the first 1,000 customers each
// their own, so a smaller scale has as many names as customers.
auto names(const Scale& s) -> std::int64_t { return std::min<std::int64_t>(1'000, s.customers); }

auto alnum(Rng& rng, std::int64_t lo, std::int64_t hi) -> std::string {
  static constexpr std::string_view kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string s(static_cast<std::size_t>(uniform(rng, lo, hi)), ' ');
  for (auto& c : s) c = kChars[static_cast<std::size_t>(uniform(rng, 0, std::ssize(kChars) - 1))];
  return s;
}

auto digits(Rng& rng, std::int64_t n) -> std::string {
  std::string s(static_cast<std::size_t>(n), '0');
  for (auto& c : s) c = static_cast<char>('0' + uniform(rng, 0, 9));
  return s;
}

auto zip(Rng& rng) -> std::string { return digits(rng, 4) + "11111"; }

// i_data and s_data: 10% hold "ORIGINAL" somewhere (clause 4.3.3.1).
auto data(Rng& rng) -> std::string {
  auto s = alnum(rng, 26, 50);
  if (uniform(rng, 1, 10) == 1) s.replace(static_cast<std::size_t>(uniform(rng, 0, std::ssize(s) - 8)), 8, "ORIGINAL");
  return s;
}

auto now_us() -> std::int64_t {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// A DECIMAL(p, 2) value as the layer takes it: its scaled integer.
auto cents(std::int64_t c) -> Value { return c; }

// history.h_id (docs/frontend_design.md, *Where the statements come from*).
constexpr std::uint64_t kMaxRun = (std::uint64_t{1} << 15) - 1;
constexpr std::size_t kMaxThreads = 256;
constexpr std::uint64_t kRowsPerThread = std::uint64_t{1} << 40;

auto h_id(std::uint64_t run, std::uint64_t thread, std::uint64_t n) -> std::int64_t {
  if (n >= kRowsPerThread) throw std::runtime_error("a thread's history ids are exhausted for this run");
  return static_cast<std::int64_t>((run << 48) | (thread << 40) | n);
}

// Loads through prepared statements, many rows to a Transaction.
class Loader {
public:
  Loader(Database& db, std::size_t rows_per_txn) : db_{db}, per_txn_{rows_per_txn} {}
  void insert(const Statement& s, std::vector<Value> params) {
    if (!txn_) txn_.emplace(db_.begin());
    (void)txn_->execute(s, params);
    if (++rows_ == per_txn_) flush();
  }
  void flush() {
    if (txn_ && txn_->commit() != CommitOutcome::Committed) throw std::runtime_error("a load transaction rolled back");
    txn_.reset();
    rows_ = 0;
  }

private:
  Database& db_;
  std::size_t per_txn_;
  std::optional<Transaction> txn_;
  std::size_t rows_{0};
};

}  // namespace

void load(Database& db, const LoadOptions& opts) {
  for (auto ddl : sql_test::kTpccSchema) (void)db.execute(ddl);
  (void)db.execute("CREATE TABLE harness_run (id INTEGER PRIMARY KEY, last_run BIGINT NOT NULL)");
  (void)db.execute("INSERT INTO harness_run (id, last_run) VALUES ($1, $2)", {std::int64_t{1}, std::int64_t{0}});

  const auto& s = opts.scale;
  Rng rng{opts.seed};
  const auto& item = db.prepare("INSERT INTO item (i_id, i_im_id, i_name, i_price, i_data) VALUES ($1, $2, $3, $4, $5)");
  const auto& warehouse = db.prepare(
      "INSERT INTO warehouse (w_id, w_ytd, w_tax, w_name, w_street_1, w_street_2, w_city, w_state, w_zip) "
      "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)");
  const auto& stock = db.prepare(
      "INSERT INTO stock (s_i_id, s_w_id, s_quantity, s_dist_01, s_dist_02, s_dist_03, s_dist_04, s_dist_05, "
      "s_dist_06, s_dist_07, s_dist_08, s_dist_09, s_dist_10, s_ytd, s_order_cnt, s_remote_cnt, s_data) "
      "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17)");
  const auto& district = db.prepare(
      "INSERT INTO district (d_id, d_w_id, d_ytd, d_tax, d_next_o_id, d_name, d_street_1, d_street_2, d_city, "
      "d_state, d_zip) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)");
  const auto& customer = db.prepare(
      "INSERT INTO customer (c_id, c_d_id, c_w_id, c_first, c_middle, c_last, c_street_1, c_street_2, c_city, "
      "c_state, c_zip, c_phone, c_since, c_credit, c_credit_lim, c_discount, c_balance, c_ytd_payment, "
      "c_payment_cnt, c_delivery_cnt, c_data) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, "
      "$15, $16, $17, $18, $19, $20, $21)");
  const auto& history = db.prepare(
      "INSERT INTO history (h_id, h_c_id, h_c_d_id, h_c_w_id, h_d_id, h_w_id, h_date, h_amount, h_data) "
      "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)");
  const auto& orders = db.prepare(
      "INSERT INTO orders (o_id, o_d_id, o_w_id, o_c_id, o_entry_d, o_carrier_id, o_ol_cnt, o_all_local) "
      "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)");
  const auto& order_line = db.prepare(
      "INSERT INTO order_line (ol_o_id, ol_d_id, ol_w_id, ol_number, ol_i_id, ol_supply_w_id, ol_delivery_d, "
      "ol_quantity, ol_amount, ol_dist_info) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)");
  const auto& new_order = db.prepare("INSERT INTO new_order (no_o_id, no_d_id, no_w_id) VALUES ($1, $2, $3)");

  Loader l{db, opts.rows_per_txn};
  const Value null{};
  const auto stamp = now_us();
  // Each customer's one history row pays 10.00, and the year-to-date sums
  // start at what those rows add up to: 30,000.00 a district and 300,000.00
  // a warehouse at the specification's scale, so conditions 8 and 9 hold at
  // any scale.
  const auto d_ytd = s.customers * 1'000;
  const auto w_ytd = kDistricts * d_ytd;
  for (std::int64_t i = 1; i <= s.items; ++i) {
    l.insert(item, {i, uniform(rng, 1, 10'000), text(alnum(rng, 14, 24)), cents(uniform(rng, 100, 10'000)),
                    text(data(rng))});
  }
  std::uint64_t history_n = 0;  // run 0, thread 0: the load's
  for (std::int64_t w = 1; w <= opts.warehouses; ++w) {
    l.insert(warehouse, {w, cents(w_ytd), uniform(rng, 0, 2'000), text(alnum(rng, 6, 10)),
                         text(alnum(rng, 10, 20)), text(alnum(rng, 10, 20)), text(alnum(rng, 10, 20)),
                         text(alnum(rng, 2, 2)), text(zip(rng))});
    for (std::int64_t i = 1; i <= s.items; ++i) {
      std::vector<Value> row{i, w, uniform(rng, 10, 100)};
      for (int d = 0; d < 10; ++d) row.push_back(text(alnum(rng, 24, 24)));
      row.insert(row.end(), {std::int64_t{0}, std::int64_t{0}, std::int64_t{0}, text(data(rng))});
      l.insert(stock, std::move(row));
    }
    for (std::int64_t d = 1; d <= kDistricts; ++d) {
      l.insert(district, {d, w, cents(d_ytd), uniform(rng, 0, 2'000), s.orders + 1, text(alnum(rng, 6, 10)),
                          text(alnum(rng, 10, 20)), text(alnum(rng, 10, 20)), text(alnum(rng, 10, 20)),
                          text(alnum(rng, 2, 2)), text(zip(rng))});
      for (std::int64_t c = 1; c <= s.customers; ++c) {
        auto last = c <= names(s) ? c - 1 : nurand(rng, 255, 0, names(s) - 1, kCLastLoad);
        auto credit = uniform(rng, 1, 10) == 1 ? "BC" : "GC";
        l.insert(customer, {c, d, w, text(alnum(rng, 8, 16)), text("OE"), text(last_name(last)),
                            text(alnum(rng, 10, 20)), text(alnum(rng, 10, 20)), text(alnum(rng, 10, 20)),
                            text(alnum(rng, 2, 2)), text(zip(rng)), text(digits(rng, 16)), stamp, text(credit),
                            cents(5'000'000), uniform(rng, 0, 5'000), cents(-1'000), cents(1'000), std::int64_t{1},
                            std::int64_t{0}, text(alnum(rng, 300, 500))});
        l.insert(history, {h_id(0, 0, history_n++), c, d, w, d, w, stamp, cents(1'000), text(alnum(rng, 12, 24))});
      }
      // Each order's customer: a permutation of the customers.
      std::vector<std::int64_t> order_customer(static_cast<std::size_t>(s.customers));
      for (std::size_t i = 0; i < order_customer.size(); ++i) order_customer[i] = static_cast<std::int64_t>(i) + 1;
      std::ranges::shuffle(order_customer, rng);
      const auto first_new = s.orders - s.new_orders + 1;
      for (std::int64_t o = 1; o <= s.orders; ++o) {
        const auto delivered = o < first_new;
        const auto lines = uniform(rng, 5, 15);
        const auto c_id = order_customer[static_cast<std::size_t>((o - 1) % s.customers)];
        l.insert(orders, {o, d, w, c_id, stamp, delivered ? Value{uniform(rng, 1, 10)} : null, lines,
                          std::int64_t{1}});
        for (std::int64_t n = 1; n <= lines; ++n) {
          l.insert(order_line, {o, d, w, n, uniform(rng, 1, s.items), w, delivered ? Value{stamp} : null,
                                std::int64_t{5}, cents(delivered ? 0 : uniform(rng, 1, 999'999)),
                                text(alnum(rng, 24, 24))});
        }
        if (!delivered) l.insert(new_order, {o, d, w});
      }
    }
  }
  l.flush();
}

auto kind_name(Kind k) -> std::string_view {
  switch (k) {
    case Kind::NewOrder: return "New-Order";
    case Kind::Payment: return "Payment";
    case Kind::OrderStatus: return "Order-Status";
    case Kind::Delivery: return "Delivery";
    case Kind::StockLevel: return "Stock-Level";
  }
  return "?";
}

void Histogram::add(std::chrono::nanoseconds d) {
  auto us = std::max<double>(1.0, static_cast<double>(d.count()) / 1'000.0);
  auto i = std::min<std::size_t>(kBuckets - 1, static_cast<std::size_t>(std::log2(us) * 8.0));
  ++buckets_[i];
  ++count_;
  max_ = std::max(max_, std::chrono::duration_cast<std::chrono::microseconds>(d));
}

void Histogram::merge(const Histogram& o) {
  for (std::size_t i = 0; i < kBuckets; ++i) buckets_[i] += o.buckets_[i];
  count_ += o.count_;
  max_ = std::max(max_, o.max_);
}

auto Histogram::quantile(double q) const -> std::chrono::microseconds {
  if (count_ == 0) return std::chrono::microseconds{0};
  auto rank = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(count_)));
  std::uint64_t seen = 0;
  for (std::size_t i = 0; i < kBuckets; ++i) {
    seen += buckets_[i];
    if (seen >= std::max<std::uint64_t>(rank, 1)) {
      auto upper = std::chrono::microseconds{static_cast<std::int64_t>(std::exp2(static_cast<double>(i + 1) / 8.0))};
      return std::min(upper, max_);
    }
  }
  return max_;
}

auto PhaseReport::tpm() const -> double {
  std::uint64_t n = 0;
  for (const auto& k : kinds) n += k.completed;
  return seconds > 0 ? static_cast<double>(n) * 60.0 / seconds : 0.0;
}

auto PhaseReport::nopm() const -> std::optional<double> {
  const auto in_mix = mix();
  if (std::ranges::find(in_mix, Kind::NewOrder) == in_mix.end()) return std::nullopt;
  return seconds > 0 ? static_cast<double>(kinds[static_cast<std::size_t>(Kind::NewOrder)].completed) * 60.0 / seconds
                     : 0.0;
}

auto mix() -> std::vector<Kind> { return {Kind::Payment, Kind::OrderStatus}; }

namespace {

// The specification's deck (clause 5.2.3), by Kind.
constexpr std::array<std::int64_t, kKinds> kWeights{45, 43, 4, 4, 4};

// The statements of docs/frontend_design.md, prepared once and shared by
// every thread.
struct Statements {
  explicit Statements(Database& db)
      : p1{db.prepare("UPDATE warehouse SET w_ytd = w_ytd + $1 WHERE w_id = $2")},
        p2{db.prepare("SELECT w_street_1, w_street_2, w_city, w_state, w_zip, w_name FROM warehouse WHERE w_id = $1")},
        p3{db.prepare("UPDATE district SET d_ytd = d_ytd + $1 WHERE d_w_id = $2 AND d_id = $3")},
        p4{db.prepare("SELECT d_street_1, d_street_2, d_city, d_state, d_zip, d_name FROM district "
                      "WHERE d_w_id = $1 AND d_id = $2")},
        p5{db.prepare("SELECT count(c_id) FROM customer WHERE c_last = $1 AND c_d_id = $2 AND c_w_id = $3")},
        p6{db.prepare("SELECT c_first, c_middle, c_id, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, "
                      "c_credit, c_credit_lim, c_discount, c_balance, c_since FROM customer WHERE c_w_id = $1 "
                      "AND c_d_id = $2 AND c_last = $3 ORDER BY c_first")},
        p7{db.prepare("SELECT c_first, c_middle, c_last, c_street_1, c_street_2, c_city, c_state, c_zip, c_phone, "
                      "c_credit, c_credit_lim, c_discount, c_balance, c_since FROM customer WHERE c_w_id = $1 "
                      "AND c_d_id = $2 AND c_id = $3")},
        p8{db.prepare("SELECT c_data FROM customer WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3")},
        p9{db.prepare("UPDATE customer SET c_balance = $1, c_data = $2 WHERE c_w_id = $3 AND c_d_id = $4 "
                      "AND c_id = $5")},
        p10{db.prepare("UPDATE customer SET c_balance = $1 WHERE c_w_id = $2 AND c_d_id = $3 AND c_id = $4")},
        p11{db.prepare("INSERT INTO history (h_id, h_c_d_id, h_c_w_id, h_c_id, h_d_id, h_w_id, h_date, h_amount, "
                       "h_data) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9)")},
        o1{p5},
        o2{db.prepare("SELECT c_balance, c_first, c_middle, c_id FROM customer WHERE c_last = $1 AND c_d_id = $2 "
                      "AND c_w_id = $3 ORDER BY c_first")},
        o3{db.prepare("SELECT c_balance, c_first, c_middle, c_last FROM customer WHERE c_id = $1 AND c_d_id = $2 "
                      "AND c_w_id = $3")},
        o4{db.prepare("SELECT o_id, o_carrier_id, o_entry_d FROM orders WHERE o_d_id = $1 AND o_w_id = $2 "
                      "AND o_c_id = $3 ORDER BY o_id DESC LIMIT 1")},
        o5{db.prepare("SELECT ol_i_id, ol_supply_w_id, ol_quantity, ol_amount, ol_delivery_d FROM order_line "
                      "WHERE ol_o_id = $1 AND ol_d_id = $2 AND ol_w_id = $3")} {}
  const Statement &p1, &p2, &p3, &p4, &p5, &p6, &p7, &p8, &p9, &p10, &p11;
  const Statement &o1, &o2, &o3, &o4, &o5;
};

// A customer: by last name, or by id.
struct Customer {
  std::int64_t w{0}, d{0};
  std::optional<std::string> last;
  std::int64_t id{0};
};

struct PaymentInput {
  std::int64_t w{0}, d{0};
  Customer c;
  std::int64_t amount{0};  // cents
  std::int64_t h_id{0};
  std::int64_t date{0};
};

struct OrderStatusInput {
  Customer c;
};

// The customer a last name selects: the one at position ceil(n / 2) by
// c_first (clause 2.5.2.2); nullopt when the name has none, which only a
// reduced scale has. `rows` from P6 or O2, `col` the column holding c_id.
auto middle(const Rows& rows, std::size_t col) -> std::optional<std::int64_t> {
  if (rows.rows.empty()) return std::nullopt;
  return i64(rows.rows[(rows.rows.size() + 1) / 2 - 1][col]);
}

auto money_text(std::int64_t cents_value) -> std::string {
  return std::format("{}.{:02}", cents_value / 100, cents_value % 100);
}

// The customer a last name selects must exist: the load gives every name
// it draws from a customer (names()).
[[noreturn]] void no_customer(const std::string& last) {
  throw std::runtime_error("no customer has the last name " + last);
}

// Payment (clause 2.5.2), as HammerDB's client-side driver runs it.
void payment(const Statements& s, Transaction& t, const PaymentInput& in) {
  (void)t.execute(s.p1, {cents(in.amount), in.w});
  auto w_name = str(t.execute(s.p2, {in.w}).rows.at(0).at(5));
  (void)t.execute(s.p3, {cents(in.amount), in.w, in.d});
  auto d_name = str(t.execute(s.p4, {in.w, in.d}).rows.at(0).at(5));
  std::int64_t c_id = in.c.id;
  Value credit, balance;
  if (in.c.last) {
    (void)t.execute(s.p5, {text(*in.c.last), in.c.d, in.c.w});
    auto rows = t.execute(s.p6, {in.c.w, in.c.d, text(*in.c.last)});
    auto picked = middle(rows, 2);
    if (!picked) no_customer(*in.c.last);
    const auto& row = rows.rows[(rows.rows.size() + 1) / 2 - 1];
    c_id = *picked;
    credit = row.at(9);
    balance = row.at(12);
  } else {
    auto rows = t.execute(s.p7, {in.c.w, in.c.d, c_id});
    const auto& row = rows.rows.at(0);
    credit = row.at(9);
    balance = row.at(12);
  }
  auto new_balance = i64(balance) - in.amount;
  if (str(credit) == "BC") {
    auto c_data = str(t.execute(s.p8, {in.c.w, in.c.d, c_id}).rows.at(0).at(0));
    auto updated = std::format("{} {} {} {} {} {} | {}", c_id, in.c.d, in.c.w, in.d, in.w, money_text(in.amount), c_data);
    if (updated.size() > 500) updated.resize(500);
    (void)t.execute(s.p9, {cents(new_balance), text(updated), in.c.w, in.c.d, c_id});
  } else {
    (void)t.execute(s.p10, {cents(new_balance), in.c.w, in.c.d, c_id});
  }
  (void)t.execute(s.p11, {in.h_id, in.c.d, in.c.w, c_id, in.d, in.w, in.date, cents(in.amount),
                          text(w_name + "    " + d_name)});
  (void)t.commit();
}

// Order-Status (clause 2.6.2): reads only.
void order_status(const Statements& s, Transaction& t, const OrderStatusInput& in) {
  std::optional<std::int64_t> c_id = in.c.id;
  if (in.c.last) {
    (void)t.execute(s.o1, {text(*in.c.last), in.c.d, in.c.w});
    c_id = middle(t.execute(s.o2, {text(*in.c.last), in.c.d, in.c.w}), 3);
    if (!c_id) no_customer(*in.c.last);
  } else {
    (void)t.execute(s.o3, {*c_id, in.c.d, in.c.w});
  }
  auto order = t.execute(s.o4, {in.c.d, in.c.w, *c_id});
  if (!order.rows.empty()) (void)t.execute(s.o5, {order.rows[0].at(0), in.c.d, in.c.w});
  (void)t.commit();
}

// Bumps harness_run for a new run and returns it (docs/frontend_design.md).
auto next_run(Database& db) -> std::uint64_t {
  const auto& bump = db.prepare("UPDATE harness_run SET last_run = last_run + $1 WHERE id = $2");
  const auto& read = db.prepare("SELECT last_run FROM harness_run WHERE id = $1");
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    try {
      auto t = db.begin();
      if (t.execute(bump, {std::int64_t{1}, std::int64_t{1}}).affected != 1) {
        throw std::runtime_error("harness_run has no row: load the database first");
      }
      auto run = static_cast<std::uint64_t>(i64(t.execute(read, {std::int64_t{1}}).rows.at(0).at(0)));
      if (t.commit() != CommitOutcome::Committed) continue;
      if (run > kMaxRun) throw std::runtime_error("harness_run is past its last run");
      return run;
    } catch (const SerializationFailure&) {
    }
  }
  throw std::runtime_error("harness_run: every attempt to bump it conflicted");
}

struct Worker {
  const Statements& s;
  std::int64_t warehouses;
  Scale scale;
  std::uint64_t run;
  std::uint64_t thread;
  Rng rng;
  std::uint64_t history_n{0};
  std::array<KindStats, kKinds> stats{};

  auto customer(std::int64_t w, std::int64_t d) -> Customer {
    Customer c{w, d, std::nullopt, 0};
    if (uniform(rng, 1, 100) <= 60) {
      c.last = last_name(nurand(rng, 255, 0, names(scale) - 1, kCLastRun));
    } else {
      c.id = nurand(rng, 1023, 1, scale.customers, kCId);
    }
    return c;
  }

  auto draw(const std::vector<Kind>& kinds) -> Kind {
    std::int64_t total = 0;
    for (auto k : kinds) total += kWeights[static_cast<std::size_t>(k)];
    auto x = uniform(rng, 0, total - 1);
    for (auto k : kinds) {
      x -= kWeights[static_cast<std::size_t>(k)];
      if (x < 0) return k;
    }
    return kinds.back();
  }

  // One TPROC-C transaction of kind k, retried on 40001 with its inputs.
  // It is counted, with its retries, when it starts at or after `from` and
  // ends before `until`: the measured window.
  void one(Database& db, Kind k, std::int64_t home, Clock::time_point from, Clock::time_point until) {
    PaymentInput pay;
    OrderStatusInput os;
    const auto d = uniform(rng, 1, kDistricts);
    switch (k) {
      case Kind::Payment: {
        pay.w = home;
        pay.d = d;
        auto c_w = home, c_d = d;
        if (warehouses > 1 && uniform(rng, 1, 100) <= 15) {
          c_w = uniform(rng, 1, warehouses - 1);
          if (c_w >= home) ++c_w;
          c_d = uniform(rng, 1, kDistricts);
        }
        pay.c = customer(c_w, c_d);
        pay.amount = uniform(rng, 100, 500'000);
        pay.h_id = h_id(run, thread, history_n++);
        pay.date = now_us();
        break;
      }
      case Kind::OrderStatus: os.c = customer(home, d); break;
      case Kind::NewOrder:
      case Kind::Delivery:
      case Kind::StockLevel: throw std::logic_error("not in the mix yet");
    }
    auto& st = stats[static_cast<std::size_t>(k)];
    const auto start = Clock::now();
    std::uint64_t retries = 0;
    auto completed = false;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
      try {
        auto t = db.begin();
        switch (k) {
          case Kind::Payment: payment(s, t, pay); break;
          case Kind::OrderStatus: order_status(s, t, os); break;
          case Kind::NewOrder:
          case Kind::Delivery:
          case Kind::StockLevel: break;
        }
        completed = true;
        break;
      } catch (const SerializationFailure&) {
        if (attempt < kMaxAttempts) ++retries;
      }
    }
    const auto end = Clock::now();
    if (start < from || end >= until) return;
    st.retries += retries;
    if (completed) {
      ++st.completed;
      st.latency.add(end - start);
    } else {
      ++st.abandoned;
    }
  }
};

auto engine_counter(Database& db, const char* name) -> std::int64_t {
  auto stats = db.engine_for_testing().stats();
  auto it = stats.find(name);
  return it == stats.end() ? 0 : it->second;
}

}  // namespace

void run(Database& db, const RunOptions& opts, const std::function<void(const PhaseReport&)>& on_phase) {
  for (auto threads : opts.threads) {
    if (threads == 0 || threads > kMaxThreads) throw std::invalid_argument("threads must be 1 to 256");
  }
  const Statements s{db};
  // The warehouses and the scale, from what the load wrote.
  auto count = [&](std::string_view sql, std::vector<Value> p) { return i64(db.execute(sql, p).rows.at(0).at(0)); };
  const auto warehouses = count("SELECT count(w_id) FROM warehouse WHERE w_id >= $1", {std::int64_t{1}});
  if (warehouses == 0) throw std::runtime_error("no warehouse: load the database first");
  Scale scale;
  scale.items = count("SELECT count(i_id) FROM item WHERE i_id >= $1", {std::int64_t{1}});
  scale.customers = count("SELECT count(c_id) FROM customer WHERE c_w_id = $1 AND c_d_id = $2",
                          {std::int64_t{1}, std::int64_t{1}});
  const auto kinds = mix();
  for (auto threads : opts.threads) {
    PhaseReport r;
    r.threads = threads;
    r.run = next_run(db);
    const auto fsyncs = engine_counter(db, "bytecask.fsyncs");
    const auto batches = engine_counter(db, "bytecask.group_writer_batches");
    const auto begin = Clock::now();
    // Counting a number of transactions measures them all.
    const auto counting = opts.txns_per_thread != 0;
    const auto measure_from = counting ? begin : begin + opts.warmup;
    const auto until = counting ? Clock::time_point::max() : measure_from + opts.duration;
    std::atomic<bool> failed{false};
    std::vector<Worker> workers;
    workers.reserve(threads);
    for (std::size_t t = 0; t < threads; ++t) {
      workers.push_back(Worker{s, warehouses, scale, r.run, t, Rng{opts.seed * 1'000'003 + t}, 0, {}});
    }
    std::vector<std::exception_ptr> errors(threads);
    std::vector<std::jthread> pool;
    for (std::size_t t = 0; t < threads; ++t) {
      pool.emplace_back([&, t] {
        try {
          auto& w = workers[t];
          const auto home = static_cast<std::int64_t>(t % static_cast<std::size_t>(warehouses)) + 1;
          for (std::uint64_t n = 0; !counting || n < opts.txns_per_thread; ++n) {
            if (failed.load() || Clock::now() >= until) break;
            w.one(db, w.draw(kinds), home, measure_from, until);
          }
        } catch (...) {
          errors[t] = std::current_exception();
          failed.store(true);
        }
      });
    }
    pool.clear();
    for (const auto& e : errors) {
      if (e) std::rethrow_exception(e);
    }
    // The window: a transaction counts if it ran inside it.
    r.seconds = std::chrono::duration<double>(counting ? Clock::now() - begin : until - measure_from).count();
    for (const auto& w : workers) {
      for (std::size_t k = 0; k < kKinds; ++k) {
        r.kinds[k].completed += w.stats[k].completed;
        r.kinds[k].retries += w.stats[k].retries;
        r.kinds[k].abandoned += w.stats[k].abandoned;
        r.kinds[k].latency.merge(w.stats[k].latency);
      }
    }
    r.fsyncs = engine_counter(db, "bytecask.fsyncs") - fsyncs;
    r.group_writer_batches = engine_counter(db, "bytecask.group_writer_batches") - batches;
    on_phase(r);
  }
}

auto consistency(Database& db) -> std::vector<std::string> {
  std::vector<std::string> bad;
  std::map<std::int64_t, std::int64_t> w_ytd, d_sum, h_by_w;
  std::map<std::pair<std::int64_t, std::int64_t>, std::int64_t> d_ytd, h_by_d;
  for (const auto& r : db.execute("SELECT w_id, w_ytd FROM warehouse WHERE w_id >= $1", {std::int64_t{1}}).rows) {
    w_ytd[i64(r.at(0))] = i64(r.at(1));
  }
  for (const auto& r :
       db.execute("SELECT d_w_id, d_id, d_ytd FROM district WHERE d_w_id >= $1", {std::int64_t{1}}).rows) {
    d_ytd[{i64(r.at(0)), i64(r.at(1))}] = i64(r.at(2));
    d_sum[i64(r.at(0))] += i64(r.at(2));
  }
  // history in pages, in h_id order, so no run holds it all in memory.
  const auto& page = db.prepare(
      "SELECT h_id, h_w_id, h_d_id, h_amount FROM history WHERE h_id >= $1 ORDER BY h_id LIMIT 10000");
  for (std::int64_t from = 0;;) {
    auto rows = db.execute(page, {from}).rows;
    for (const auto& r : rows) {
      h_by_w[i64(r.at(1))] += i64(r.at(3));
      h_by_d[{i64(r.at(1)), i64(r.at(2))}] += i64(r.at(3));
    }
    if (rows.size() < 10'000) break;
    from = i64(rows.back().at(0)) + 1;
  }
  for (const auto& [w, ytd] : w_ytd) {
    if (ytd != d_sum[w]) {
      bad.push_back(std::format("condition 1: warehouse {} has w_ytd {}, its districts' d_ytd sum to {}", w,
                                money_text(ytd), money_text(d_sum[w])));
    }
    if (ytd != h_by_w[w]) {
      bad.push_back(std::format("condition 8: warehouse {} has w_ytd {}, its history sums to {}", w, money_text(ytd),
                                money_text(h_by_w[w])));
    }
  }
  for (const auto& [wd, ytd] : d_ytd) {
    if (ytd != h_by_d[wd]) {
      bad.push_back(std::format("condition 9: district {} of warehouse {} has d_ytd {}, its history sums to {}",
                                wd.second, wd.first, money_text(ytd), money_text(h_by_d[wd])));
    }
  }
  return bad;
}

auto format_report(const PhaseReport& r) -> std::string {
  auto out = std::format("threads {}  run {}  {:.1f} s  TPM {:.0f}  NOPM {}\n", r.threads, r.run, r.seconds, r.tpm(),
                         r.nopm() ? std::format("{:.0f}", *r.nopm()) : std::string{"n/a"});
  out += std::format("  {:<13}{:>10}{:>9}{:>10}{:>10}{:>10}{:>10}{:>10}\n", "transaction", "completed", "retries",
                     "abandoned", "p50 us", "p95 us", "p99 us", "max us");
  for (auto k : mix()) {
    const auto& s = r.kinds[static_cast<std::size_t>(k)];
    out += std::format("  {:<13}{:>10}{:>9}{:>10}{:>10}{:>10}{:>10}{:>10}\n", kind_name(k), s.completed, s.retries,
                       s.abandoned, s.latency.quantile(0.50).count(), s.latency.quantile(0.95).count(),
                       s.latency.quantile(0.99).count(), s.latency.max().count());
  }
  out += std::format("  engine: {} fsyncs, {} group-commit batches\n", r.fsyncs, r.group_writer_batches);
  return out;
}

auto json_report(const PhaseReport& r) -> std::string {
  auto out = std::format(R"({{"threads":{},"run":{},"seconds":{:.3f},"tpm":{:.1f},"nopm":{},"fsyncs":{},)"
                         R"("group_writer_batches":{},"transactions":{{)",
                         r.threads, r.run, r.seconds, r.tpm(), r.nopm() ? std::format("{:.1f}", *r.nopm()) : "null",
                         r.fsyncs, r.group_writer_batches);
  auto first = true;
  for (auto k : mix()) {
    const auto& s = r.kinds[static_cast<std::size_t>(k)];
    out += std::format(R"({}"{}":{{"completed":{},"retries":{},"abandoned":{},"p50_us":{},"p95_us":{},)"
                       R"("p99_us":{},"max_us":{}}})",
                       first ? "" : ",", kind_name(k), s.completed, s.retries, s.abandoned,
                       s.latency.quantile(0.50).count(), s.latency.quantile(0.95).count(),
                       s.latency.quantile(0.99).count(), s.latency.max().count());
    first = false;
  }
  return out + "}}";
}

namespace {

// ---------------------------------------------------------------- Elle

// One micro-operation: a read of k, or an append of element e to k.
struct Mop {
  bool append{false};
  std::int64_t key{0};
  std::int64_t element{0};
  std::vector<std::int64_t> read;  // what a read, or an append's own read, saw
};

struct ElleOp {
  std::uint64_t index{0};  // the history's order: taken as the entry is logged
  std::string type;        // invoke, ok, fail, info
  std::uint64_t process{0};
  std::int64_t time{0};  // ns since the history's start
  std::vector<Mop> mops;
};

auto parse_list(const std::string& v) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> out;
  std::size_t at = 0;
  while (at < v.size()) {
    auto comma = v.find(',', at);
    if (comma == std::string::npos) comma = v.size();
    out.push_back(std::stoll(v.substr(at, comma - at)));
    at = comma + 1;
  }
  return out;
}

auto list_text(const std::vector<std::int64_t>& l) -> std::string {
  std::string s;
  for (auto e : l) s += (s.empty() ? "" : ",") + std::to_string(e);
  return s;
}

auto mops_json(const std::vector<Mop>& mops, bool completed) -> std::string {
  std::string out = "[";
  for (const auto& m : mops) {
    if (out.size() > 1) out += ",";
    if (m.append) {
      out += std::format(R"(["append",{},{}])", m.key, m.element);
    } else if (completed) {
      out += std::format(R"(["r",{},[{}]])", m.key, list_text(m.read));
    } else {
      out += std::format(R"(["r",{},null])", m.key);
    }
  }
  return out + "]";
}

constexpr std::int64_t kActiveKeys = 8;
constexpr std::int64_t kAppendsPerKey = 64;

}  // namespace

auto elle_history(Database& db, const ElleOptions& opts) -> std::string {
  (void)db.execute("CREATE TABLE elle (k BIGINT PRIMARY KEY, v VARCHAR(2048) NOT NULL)");
  // Every key a run can reach exists before the clients start, so every
  // read returns a row (and FOR UPDATE has a row to guard).
  const auto keys = static_cast<std::int64_t>(opts.txns) * 4 / kAppendsPerKey + kActiveKeys + 1;
  {
    const auto& ins = db.prepare("INSERT INTO elle (k, v) VALUES ($1, $2)");
    Loader l{db, 2'000};
    for (std::int64_t k = 0; k < keys; ++k) l.insert(ins, {k, text("")});
    l.flush();
  }
  const auto& read = db.prepare("SELECT v FROM elle WHERE k = $1");
  const auto& lock = db.prepare("SELECT v FROM elle WHERE k = $1 FOR UPDATE");
  const auto& write = db.prepare("UPDATE elle SET v = $2 WHERE k = $1");

  // Key rotation, shared by the clients: slot i holds key active[i] until it
  // has taken kAppendsPerKey appends.
  std::mutex mu;
  std::vector<std::int64_t> active(kActiveKeys), appends(kActiveKeys, 0);
  for (std::int64_t i = 0; i < kActiveKeys; ++i) active[static_cast<std::size_t>(i)] = i;
  std::int64_t next_key = kActiveKeys;
  std::atomic<std::int64_t> next_element{1};
  std::atomic<std::int64_t> remaining{static_cast<std::int64_t>(opts.txns)};
  std::atomic<std::uint64_t> next_index{0};
  const auto start = Clock::now();
  auto now_ns = [&] { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count(); };

  std::vector<std::vector<ElleOp>> logs(opts.threads);
  std::vector<std::exception_ptr> errors(opts.threads);
  {
    std::vector<std::jthread> pool;
    for (std::size_t t = 0; t < opts.threads; ++t) {
      pool.emplace_back([&, t] {
        try {
          Rng rng{opts.seed * 7'919 + t};
          auto process = static_cast<std::uint64_t>(t);
          while (remaining.fetch_sub(1) > 0) {
            std::vector<Mop> mops(static_cast<std::size_t>(uniform(rng, 1, 4)));
            {
              std::lock_guard<std::mutex> g{mu};
              for (auto& m : mops) {
                auto slot = static_cast<std::size_t>(uniform(rng, 0, kActiveKeys - 1));
                m.key = active[slot];
                m.append = uniform(rng, 0, 1) == 1;
                if (m.append) {
                  m.element = next_element.fetch_add(1);
                  if (++appends[slot] == kAppendsPerKey) {
                    active[slot] = next_key++;
                    appends[slot] = 0;
                  }
                }
              }
            }
            auto& log = logs[t];
            log.push_back({next_index.fetch_add(1), "invoke", process, now_ns(), mops});
            std::string outcome = "ok";
            try {
              std::optional<Transaction> txn;
              if (opts.config != ElleConfig::Blind) txn.emplace(db.begin());
              auto exec = [&](const Statement& s, std::vector<Value> p) {
                return txn ? txn->execute(s, p) : db.execute(s, p);
              };
              // Within a transaction a read sees the transaction's own
              // earlier appends: the view reads its earlier statements.
              for (auto& m : mops) {
                auto appended_here = std::ranges::any_of(mops, [&](const Mop& o) { return o.append && o.key == m.key; });
                const auto& s = opts.config == ElleConfig::Guarded && !appended_here ? lock : read;
                auto v = parse_list(str(exec(s, {m.key}).rows.at(0).at(0)));
                if (m.append) {
                  auto with = v;
                  with.push_back(m.element);
                  (void)exec(write, {m.key, text(list_text(with))});
                } else {
                  m.read = std::move(v);
                }
              }
              if (txn && txn->commit() != CommitOutcome::Committed) outcome = "fail";
            } catch (const SerializationFailure&) {
              outcome = "fail";
            } catch (const ExecError& e) {
              // In autocommit, a statement that conflicted on every retry:
              // the statements before it committed.
              if (opts.config != ElleConfig::Blind || !e.conflict()) throw;
              outcome = "info";
            } catch (const TransactionResolutionUnknown&) {
              outcome = "info";
            }
            log.push_back({next_index.fetch_add(1), outcome, process, now_ns(), mops});
            // A process with an :info entry cannot go on (Elle's rule): take a fresh one.
            if (outcome == "info") process += opts.threads;
          }
        } catch (...) {
          errors[t] = std::current_exception();
        }
      });
    }
  }
  for (const auto& e : errors) {
    if (e) std::rethrow_exception(e);
  }
  // The final read of every key, one transaction, after the clients.
  const auto final_process = static_cast<std::uint64_t>(opts.threads) * 1'000'000;
  ElleOp final_read{0, "ok", final_process, now_ns(), {}};
  {
    auto t = db.begin();
    for (std::int64_t k = 0; k < keys; ++k) {
      Mop m;
      m.key = k;
      m.read = parse_list(str(t.execute(read, {k}).rows.at(0).at(0)));
      if (!m.read.empty()) final_read.mops.push_back(std::move(m));
    }
    (void)t.commit();
  }
  std::vector<ElleOp> all;
  for (auto& l : logs) all.insert(all.end(), l.begin(), l.end());
  std::ranges::sort(all, {}, &ElleOp::index);
  auto invoke = final_read;
  invoke.type = "invoke";
  all.push_back(invoke);
  all.push_back(final_read);
  std::string out = "[\n";
  for (std::size_t i = 0; i < all.size(); ++i) {
    const auto& op = all[i];
    out += std::format(R"({{"index":{},"process":{},"type":"{}","f":"txn","time":{},"value":{}}}{})", i,
                       op.process, op.type, op.time, mops_json(op.mops, op.type == "ok"),
                       i + 1 == all.size() ? "\n" : ",\n");
  }
  return out + "]\n";
}

}  // namespace bytecask::sql::tpcc
