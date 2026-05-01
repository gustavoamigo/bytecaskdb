// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// bytecaskdb_plugin.cc — MariaDB plugin registration for ByteCaskDB.
//
// Implements the handlerton lifecycle:
//   bytecaskdb_init()   — opens the global DB, rebuilds catalog cache.
//   bytecaskdb_deinit() — closes the global DB.
//
// Persistent catalog: table metadata and ID counters are stored inside
// ByteCaskDB under the 0x01 namespace.  An in-memory cache (name → id,
// id → meta) is rebuilt at startup by scanning the catalog key range.

#include "my_global.h"
#include "handler.h"
#include "mysql/plugin.h"

#include "ha_bytecaskdb.h"
#include "catalog.h"
#include "bytecaskdb_txn.h"
#include "bytecask_view.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace bytecaskdb {

// Holder for the engine instance. `bytecask::DB` is non-copyable AND
// non-moveable (relies on mandatory copy elision in `DB::open`), so it cannot
// live inside `std::unique_ptr` or `std::optional` directly. Wrapping it in a
// struct whose constructor performs the open allows us to manage its lifetime
// through `std::unique_ptr<DBHolder>` instead.
struct DBHolder {
  bytecask::DB db;
  DBHolder(const std::string &dir, bytecask::Options opts)
      : db{bytecask::DB::open(dir, std::move(opts))} {}
};

// Definitions for the globals declared extern in ha_bytecaskdb.h.
std::unique_ptr<DBHolder>      g_db_owner;
bytecask::DB                  *g_db           = nullptr;
handlerton                    *bytecaskdb_hton = nullptr;

// ---------------------------------------------------------------------------
// Persistent catalog — in-memory caches rebuilt at startup.
// ---------------------------------------------------------------------------

static std::mutex                          s_catalog_mu;
static std::map<std::string, uint32_t>     s_name_to_id;
static std::map<uint32_t, TableMeta>       s_id_to_meta;

// Per-table synthetic rowid counters for tables without a PRIMARY KEY.
// Map insertion is guarded by s_rowid_counters_mu; reads/fetch_add on an
// existing entry are lock-free via std::atomic.
static std::mutex                                       s_rowid_counters_mu;
static std::map<uint32_t, std::unique_ptr<std::atomic<uint64_t>>>
                                                        s_rowid_counters;

static std::atomic<uint64_t> &rowid_counter_for(uint32_t table_id) {
  std::lock_guard<std::mutex> lk{s_rowid_counters_mu};
  auto it = s_rowid_counters.find(table_id);
  if (it == s_rowid_counters.end()) {
    auto [ins, _] = s_rowid_counters.emplace(
        table_id, std::make_unique<std::atomic<uint64_t>>(0));
    it = ins;
  }
  return *it->second;
}

uint64_t catalog_alloc_rowid(uint32_t table_id) {
  return rowid_counter_for(table_id).fetch_add(1) + 1;
}

// Atomically reserves `count` consecutive values. Returns the first value.
uint64_t catalog_alloc_rowid_range(uint32_t table_id, uint64_t count) {
  if (count == 0) { count = 1; }
  return rowid_counter_for(table_id).fetch_add(count) + 1;
}

uint64_t catalog_peek_rowid(uint32_t table_id) {
  return rowid_counter_for(table_id).load();
}

void catalog_seed_rowid(uint32_t table_id, uint64_t high_water) {
  auto &c = rowid_counter_for(table_id);
  // Only raise the counter; never lower it.
  uint64_t expected = c.load();
  while (high_water > expected &&
         !c.compare_exchange_weak(expected, high_water)) {
    // expected updated by CAS failure; loop.
  }
}

void catalog_drop_rowid(uint32_t table_id) {
  std::lock_guard<std::mutex> lk{s_rowid_counters_mu};
  s_rowid_counters.erase(table_id);
}

// Unconditionally sets the counter to `value` — for TRUNCATE / explicit reset.
void catalog_reset_rowid(uint32_t table_id, uint64_t value) {
  rowid_counter_for(table_id).store(value);
}

// ---------------------------------------------------------------------------
// Per-table AUTO_INCREMENT counters — independent of synthetic rowid counters.
// ---------------------------------------------------------------------------

static std::mutex                                       s_autoinc_counters_mu;
static std::map<uint32_t, std::unique_ptr<std::atomic<uint64_t>>>
                                                        s_autoinc_counters;

static std::atomic<uint64_t> &autoinc_counter_for(uint32_t table_id) {
  std::lock_guard<std::mutex> lk{s_autoinc_counters_mu};
  auto it = s_autoinc_counters.find(table_id);
  if (it == s_autoinc_counters.end()) {
    auto [ins, _] = s_autoinc_counters.emplace(
        table_id, std::make_unique<std::atomic<uint64_t>>(0));
    it = ins;
  }
  return *it->second;
}

uint64_t catalog_alloc_autoinc_range(uint32_t table_id, uint64_t count) {
  if (count == 0) { count = 1; }
  return autoinc_counter_for(table_id).fetch_add(count) + 1;
}

uint64_t catalog_peek_autoinc(uint32_t table_id) {
  return autoinc_counter_for(table_id).load();
}

void catalog_seed_autoinc(uint32_t table_id, uint64_t high_water) {
  auto &c = autoinc_counter_for(table_id);
  uint64_t expected = c.load();
  while (high_water > expected &&
         !c.compare_exchange_weak(expected, high_water)) {
  }
}

void catalog_reset_autoinc(uint32_t table_id, uint64_t value) {
  autoinc_counter_for(table_id).store(value);
}

void catalog_drop_autoinc(uint32_t table_id) {
  std::lock_guard<std::mutex> lk{s_autoinc_counters_mu};
  s_autoinc_counters.erase(table_id);
}

// Rebuilds the in-memory caches by scanning the catalog key range. Called
// once during plugin init.
static bool catalog_init(bytecask::DB *db) {
  auto [lower, upper] = table_meta_scan_bounds();
  try {
    for (auto &[k, v] : db->iter_from({}, as_view(lower.data(), lower.size()))) {
      // Stop if past the upper bound.
      const auto *kp = u8_data(k);
      if (k.size() < upper.size() ||
          std::memcmp(kp, upper.data(), upper.size()) >= 0) {
        break;
      }

      TableMeta meta;
      if (deserialize_table_meta(u8_data(v), v.size(), meta)) {
        s_name_to_id.emplace(meta.full_name, meta.table_id);
        s_id_to_meta.emplace(meta.table_id, std::move(meta));
      }
    }
  } catch (const std::exception &e) {
    fprintf(stderr, "[ha_bytecaskdb] Catalog scan failed: %s\n", e.what());
    return false;
  }

  fprintf(stderr, "[ha_bytecaskdb] Catalog loaded: %zu tables\n",
          s_name_to_id.size());
  return true;
}

// Allocates a new table_id atomically using a snapshot + write plan with
// auto-conflict detection on the counter key. Retries on conflict.
uint32_t catalog_alloc_table_id(bytecask::DB *db) {
  auto ckey = counter_key(kCounterTableId);

  for (;;) {
    bytecask::Snapshot snap = db->snapshot();

    bytecask::Bytes val;
    uint64_t current_id = 0;
    try {
      if (snap.get({}, as_view(ckey.data(), ckey.size()), val)) {
        current_id = decode_counter_value(u8_data(val), val.size());
      }
    } catch (const std::exception &e) {
      fprintf(stderr, "[ha_bytecaskdb] alloc_table_id read failed: %s\n",
              e.what());
      return 0;
    }

    uint64_t new_id = current_id + 1;
    auto new_val = encode_counter_value(new_id);

    bytecask::WritePlan plan{std::move(snap)};
    plan.put(as_view(ckey.data(), ckey.size()),
             as_view(new_val.data(), new_val.size()));

    bool committed = false;
    try {
      committed = db->apply_batch(bytecask::WriteOptions{.sync = true},
                                  std::move(plan));
    } catch (const std::exception &e) {
      fprintf(stderr, "[ha_bytecaskdb] alloc_table_id apply failed: %s\n",
              e.what());
      return 0;
    }

    if (committed) {
      return static_cast<uint32_t>(new_id);
    }
    // conflict: retry
  }
}

// Persists table metadata and updates in-memory caches.
bool catalog_put_table_meta(bytecask::DB *db, const TableMeta &meta,
                            const char *name) {
  auto normalized = normalize_table_name(name);
  auto key = table_meta_key(name);
  auto val = serialize_table_meta(meta);

  try {
    db->put(bytecask::WriteOptions{.sync = true},
            as_view(key.data(), key.size()),
            as_view(val.data(), val.size()));
  } catch (const std::exception &e) {
    fprintf(stderr, "[ha_bytecaskdb] put_table_meta failed: %s\n", e.what());
    return false;
  }

  std::lock_guard<std::mutex> lk{s_catalog_mu};
  s_name_to_id[normalized] = meta.table_id;
  s_id_to_meta[meta.table_id] = meta;
  return true;
}

// Deletes table metadata and updates in-memory caches.
bool catalog_delete_table_meta(bytecask::DB *db, const char *name) {
  auto normalized = normalize_table_name(name);
  auto key = table_meta_key(name);

  try {
    (void)db->del(bytecask::WriteOptions{.sync = true},
                  as_view(key.data(), key.size()));
  } catch (const std::exception &e) {
    fprintf(stderr, "[ha_bytecaskdb] delete_table_meta failed: %s\n",
            e.what());
    return false;
  }

  std::lock_guard<std::mutex> lk{s_catalog_mu};
  auto it = s_name_to_id.find(normalized);
  if (it != s_name_to_id.end()) {
    catalog_drop_rowid(it->second);
    catalog_drop_autoinc(it->second);
    s_id_to_meta.erase(it->second);
    s_name_to_id.erase(it);
  }
  return true;
}

// Evicts a table from the in-memory catalog cache without touching the DB.
// Used when the catalog key is already deleted as part of an atomic WritePlan.
void catalog_evict_from_cache(const char *name) {
  auto normalized = normalize_table_name(name);
  std::lock_guard<std::mutex> lk{s_catalog_mu};
  auto it = s_name_to_id.find(normalized);
  if (it != s_name_to_id.end()) {
    catalog_drop_rowid(it->second);
    catalog_drop_autoinc(it->second);
    s_id_to_meta.erase(it->second);
    s_name_to_id.erase(it);
  }
}

// Atomically renames table metadata: deletes old key, inserts new key.
bool catalog_rename_table_meta(bytecask::DB *db,
                               const char *from, const char *to) {
  auto old_normalized = normalize_table_name(from);
  auto new_normalized = normalize_table_name(to);
  auto old_key = table_meta_key(from);
  auto new_key = table_meta_key(to);

  // Read existing metadata.
  bytecask::Bytes val;
  try {
    if (!db->get({}, as_view(old_key.data(), old_key.size()), val)) {
      return false;
    }
  } catch (const std::exception &e) {
    fprintf(stderr, "[ha_bytecaskdb] rename_table_meta read failed: %s\n",
            e.what());
    return false;
  }

  TableMeta meta;
  if (!deserialize_table_meta(u8_data(val), val.size(), meta)) {
    return false;
  }

  meta.full_name = new_normalized;
  auto new_val = serialize_table_meta(meta);

  // Atomic: delete old + put new.
  bytecask::WritePlan plan;
  plan.del(as_view(old_key.data(), old_key.size()));
  plan.put(as_view(new_key.data(), new_key.size()),
           as_view(new_val.data(), new_val.size()));

  bool committed = false;
  try {
    committed = db->apply_batch(bytecask::WriteOptions{.sync = true},
                                std::move(plan));
  } catch (const std::exception &e) {
    fprintf(stderr, "[ha_bytecaskdb] rename_table_meta apply failed: %s\n",
            e.what());
    return false;
  }
  if (!committed) {
    return false;
  }

  std::lock_guard<std::mutex> lk{s_catalog_mu};
  auto it = s_name_to_id.find(old_normalized);
  if (it != s_name_to_id.end()) {
    uint32_t tid = it->second;
    s_name_to_id.erase(it);
    s_name_to_id[new_normalized] = tid;
    if (auto mit = s_id_to_meta.find(tid); mit != s_id_to_meta.end()) {
      mit->second.full_name = new_normalized;
    }
  }
  return true;
}

// In-memory lookups (fast path).
std::optional<uint32_t> catalog_lookup_table_id(const char *name) {
  auto normalized = normalize_table_name(name);
  std::lock_guard<std::mutex> lk{s_catalog_mu};
  auto it = s_name_to_id.find(normalized);
  if (it != s_name_to_id.end()) {
    return it->second;
  }
  return std::nullopt;
}

const TableMeta *catalog_lookup_meta(uint32_t table_id) {
  std::lock_guard<std::mutex> lk{s_catalog_mu};
  auto it = s_id_to_meta.find(table_id);
  if (it != s_id_to_meta.end()) {
    return &it->second;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Handler factory — called by MariaDB for every open/create
// ---------------------------------------------------------------------------

static handler *bytecaskdb_create_handler(handlerton *hton,
                                          TABLE_SHARE *table,
                                          MEM_ROOT *mem_root) {
  return new (mem_root) ha_bytecaskdb(hton, table);
}

// ---------------------------------------------------------------------------
// Handlerton callbacks — commit / rollback / close_connection
// ---------------------------------------------------------------------------

static int bytecaskdb_commit(handlerton *hton, THD *thd, bool all) {
  auto *txn = static_cast<MariaDBTxn *>(thd_get_ha_data(thd, hton));
  if (!txn) { return 0; }
  return txn->commit(thd, all);
}

static int bytecaskdb_rollback(handlerton *hton, THD *thd, bool all) {
  auto *txn = static_cast<MariaDBTxn *>(thd_get_ha_data(thd, hton));
  if (!txn) { return 0; }
  txn->rollback(thd, all);
  return 0;
}

static int bytecaskdb_close_connection(handlerton *hton, THD *thd) {
  auto *txn = static_cast<MariaDBTxn *>(thd_get_ha_data(thd, hton));
  if (txn) {
    delete txn;
    thd_set_ha_data(thd, hton, nullptr);
  }
  return 0;
}

static int bytecaskdb_start_consistent_snapshot(handlerton *hton, THD *thd) {
  // Create a snapshot for consistent reads (needed for mysqldump --single-transaction)
  if (!g_db) { return 1; }

  auto *txn = static_cast<MariaDBTxn *>(thd_get_ha_data(thd, hton));
  if (!txn) {
    txn = new MariaDBTxn(g_db);
    thd_set_ha_data(thd, hton, txn);
  }

  // Ensure the transaction has a snapshot for consistent reads
  txn->begin_if_needed(thd, hton);
  return 0;
}

// ---------------------------------------------------------------------------
// Plugin init / deinit
// ---------------------------------------------------------------------------

static int bytecaskdb_init(void *p) {
  auto *hton = static_cast<handlerton *>(p);
  bytecaskdb_hton = hton;

  hton->create                   = bytecaskdb_create_handler;
  hton->commit                   = bytecaskdb_commit;
  hton->rollback                 = bytecaskdb_rollback;
  hton->close_connection         = bytecaskdb_close_connection;
  hton->start_consistent_snapshot = bytecaskdb_start_consistent_snapshot;
  hton->flags                    = HTON_NO_FLAGS;

  // Open the global database inside MariaDB's data directory.
  std::string db_path = std::string(mysql_real_data_home) + "bytecaskdb";

  bytecask::Options opts;
  opts.recovery_threads = 4;

  try {
    g_db_owner = std::make_unique<DBHolder>(db_path, opts);
    g_db = &g_db_owner->db;
  } catch (const std::exception &e) {
    fprintf(stderr, "[ha_bytecaskdb] Failed to open global DB at '%s': %s\n",
            db_path.c_str(), e.what());
    return 1;
  }

  if (!catalog_init(g_db)) {
    fprintf(stderr, "[ha_bytecaskdb] Failed to initialize catalog\n");
    g_db = nullptr;
    g_db_owner.reset();
    return 1;
  }

  fprintf(stderr, "[ha_bytecaskdb] Opened global DB at '%s'\n",
          db_path.c_str());
  return 0;
}

static int bytecaskdb_deinit(void * /*p*/) {
  g_db = nullptr;
  g_db_owner.reset();
  // Clear caches.
  {
    std::lock_guard<std::mutex> lk{s_catalog_mu};
    s_name_to_id.clear();
    s_id_to_meta.clear();
  }
  {
    std::lock_guard<std::mutex> lk{s_rowid_counters_mu};
    s_rowid_counters.clear();
  }
  {
    std::lock_guard<std::mutex> lk{s_autoinc_counters_mu};
    s_autoinc_counters.clear();
  }
  return 0;
}

} // namespace bytecaskdb

// ---------------------------------------------------------------------------
// MariaDB / MySQL plugin descriptor
// ---------------------------------------------------------------------------

struct st_mysql_storage_engine bytecaskdb_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

maria_declare_plugin(ha_bytecaskdb) {
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &bytecaskdb_storage_engine,
    "bytecaskdb",
    "ByteCaskDB Authors",
    "ByteCaskDB storage engine",
    PLUGIN_LICENSE_GPL,
    bytecaskdb::bytecaskdb_init,
    bytecaskdb::bytecaskdb_deinit,
    0x0002,           // version 0.2
    nullptr,          // status variables
    nullptr,          // system variables
    "0.2",            // version string
    MariaDB_PLUGIN_MATURITY_GAMMA,
} maria_declare_plugin_end;
