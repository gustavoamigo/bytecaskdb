// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// sql_layer_tpcc — the MVP gate harness (docs/testing_design.md, *The MVP
// gate harness*).
//
//   sql_layer_tpcc load --dir D [--warehouses W] [--seed N]
//   sql_layer_tpcc run  --dir D [--threads 1,16,48] [--warmup S] [--duration S] [--seed N] [--json FILE]
//   sql_layer_tpcc elle --dir D --config guarded|unguarded|blind [--threads N] [--txns N] [--seed N] --out FILE
//
// run checks TPC-C's consistency conditions after its last phase. Any error
// but a retried 40001, and any failed condition, exits non-zero.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

import bytecask.sql.api;
import bytecask.sql.tpcc;

namespace tpcc = bytecask::sql::tpcc;

namespace {

auto flags(int argc, char** argv) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
  if (argc % 2 != 0) throw std::invalid_argument(std::string{"a flag without a value: "} + argv[argc - 1]);
  for (int i = 2; i + 1 < argc; i += 2) {
    std::string_view k{argv[i]};
    if (!k.starts_with("--")) throw std::invalid_argument("expected --flag value, got " + std::string{k});
    out[std::string{k.substr(2)}] = argv[i + 1];
  }
  return out;
}

auto get(const std::map<std::string, std::string>& f, const std::string& k, const std::string& def) -> std::string {
  auto it = f.find(k);
  if (it != f.end()) return it->second;
  if (def.empty()) throw std::invalid_argument("--" + k + " is required");
  return def;
}

// A flag's value as To, refused outside To's range.
template <typename To>
auto number(const std::map<std::string, std::string>& f, const std::string& k, const std::string& def) -> To {
  auto v = std::stoull(get(f, k, def));
  if (!std::in_range<To>(v)) throw std::invalid_argument("--" + k + " is out of range");
  return static_cast<To>(v);
}

// Opens FILE for writing or appending, or throws: a run's output is never
// dropped silently.
auto output(const std::string& path, std::ios::openmode mode) -> std::ofstream {
  std::ofstream out{path, mode};
  if (!out) throw std::runtime_error("cannot write " + path);
  return out;
}

auto list(const std::string& s) -> std::vector<std::size_t> {
  std::vector<std::size_t> out;
  std::size_t at = 0;
  while (at <= s.size()) {
    auto comma = s.find(',', at);
    if (comma == std::string::npos) comma = s.size();
    out.push_back(std::stoull(s.substr(at, comma - at)));
    at = comma + 1;
  }
  return out;
}

auto run_main(int argc, char** argv) -> int {
  if (argc < 2) {
    std::fputs("usage: sql_layer_tpcc load|run|elle --dir D [...]\n", stderr);
    return 2;
  }
  const std::string_view cmd{argv[1]};
  const auto f = flags(argc, argv);
  const std::filesystem::path dir{get(f, "dir", "")};
  if (cmd == "load") {
    if (std::filesystem::exists(dir) && !std::filesystem::is_empty(dir)) {
      throw std::invalid_argument(dir.string() + " is not empty: load needs a new database");
    }
    auto db = bytecask::sql::Database::open(dir);
    const auto start = std::chrono::steady_clock::now();
    tpcc::load(*db, {.warehouses = number<std::int64_t>(f, "warehouses", "1"),
                     .scale = {},
                     .seed = number<std::uint64_t>(f, "seed", "1"),
                     .rows_per_txn = 2'000});
    db->close();
    std::printf("loaded in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return 0;
  }
  if (cmd == "run") {
    tpcc::RunOptions opts;
    opts.threads = list(get(f, "threads", "1,16,48"));
    opts.warmup = std::chrono::seconds{number<std::int64_t>(f, "warmup", "10")};
    opts.duration = std::chrono::seconds{number<std::int64_t>(f, "duration", "60")};
    opts.seed = number<std::uint64_t>(f, "seed", "1");
    std::optional<std::ofstream> json;
    if (f.contains("json")) json = output(f.at("json"), std::ios::app);
    auto db = bytecask::sql::Database::open(dir);
    // Each phase is reported as it ends, so a later phase's failure loses
    // none of the earlier ones.
    tpcc::run(*db, opts, [&](const tpcc::PhaseReport& r) {
      std::fputs(tpcc::format_report(r).c_str(), stdout);
      std::fflush(stdout);
      if (json) {
        *json << tpcc::json_report(r) << "\n" << std::flush;
        if (!*json) throw std::runtime_error("cannot write " + f.at("json"));
      }
    });
    auto bad = tpcc::consistency(*db);
    db->close();
    for (const auto& b : bad) std::fprintf(stderr, "consistency: %s\n", b.c_str());
    if (!bad.empty()) return 1;
    std::puts("consistency conditions hold");
    return 0;
  }
  if (cmd == "elle") {
    const auto config = get(f, "config", "");
    tpcc::ElleOptions opts;
    opts.config = config == "guarded"     ? tpcc::ElleConfig::Guarded
                  : config == "unguarded" ? tpcc::ElleConfig::Unguarded
                  : config == "blind"     ? tpcc::ElleConfig::Blind
                                          : throw std::invalid_argument("--config is guarded, unguarded or blind");
    opts.threads = number<std::size_t>(f, "threads", "8");
    opts.txns = number<std::uint64_t>(f, "txns", "2000");
    opts.seed = number<std::uint64_t>(f, "seed", "1");
    auto out = output(get(f, "out", ""), std::ios::trunc);
    auto db = bytecask::sql::Database::open(dir);
    auto history = tpcc::elle_history(*db, opts);
    db->close();
    out << history << std::flush;
    if (!out) throw std::runtime_error("cannot write " + f.at("out"));
    return 0;
  }
  std::fprintf(stderr, "unknown command %s\n", argv[1]);
  return 2;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  try {
    return run_main(argc, argv);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "sql_layer_tpcc: %s\n", e.what());
    return 1;
  }
}
