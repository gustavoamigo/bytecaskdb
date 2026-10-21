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
#include <string>
#include <string_view>
#include <vector>

import bytecask.sql.api;
import bytecask.sql.tpcc;

namespace tpcc = bytecask::sql::tpcc;

namespace {

auto flags(int argc, char** argv) -> std::map<std::string, std::string> {
  std::map<std::string, std::string> out;
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

auto number(const std::map<std::string, std::string>& f, const std::string& k, const std::string& def)
    -> std::uint64_t {
  return std::stoull(get(f, k, def));
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
    tpcc::load(*db, {.warehouses = static_cast<std::int64_t>(number(f, "warehouses", "1")),
                     .scale = {},
                     .seed = number(f, "seed", "1"),
                     .rows_per_txn = 2'000});
    db->close();
    std::printf("loaded in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return 0;
  }
  if (cmd == "run") {
    auto db = bytecask::sql::Database::open(dir);
    tpcc::RunOptions opts;
    opts.threads = list(get(f, "threads", "1,16,48"));
    opts.warmup = std::chrono::seconds{number(f, "warmup", "10")};
    opts.duration = std::chrono::seconds{number(f, "duration", "60")};
    opts.seed = number(f, "seed", "1");
    const auto json = f.contains("json") ? f.at("json") : std::string{};
    for (const auto& r : tpcc::run(*db, opts)) {
      std::fputs(tpcc::format_report(r).c_str(), stdout);
      std::fflush(stdout);
      if (!json.empty()) std::ofstream{json, std::ios::app} << tpcc::json_report(r) << "\n";
    }
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
    opts.threads = static_cast<std::size_t>(number(f, "threads", "8"));
    opts.txns = number(f, "txns", "2000");
    opts.seed = number(f, "seed", "1");
    auto db = bytecask::sql::Database::open(dir);
    auto history = tpcc::elle_history(*db, opts);
    db->close();
    std::ofstream{get(f, "out", "")} << history;
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
