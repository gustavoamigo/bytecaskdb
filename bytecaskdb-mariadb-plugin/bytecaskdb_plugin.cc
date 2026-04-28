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

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace bytecaskdb {

// Definitions for the globals declared extern in ha_bytecaskdb.h.
bytecask_db_t *g_db           = nullptr;
handlerton    *bytecaskdb_hton = nullptr;

// ---------------------------------------------------------------------------
// Persistent catalog — in-memory caches rebuilt at startup.
// ---------------------------------------------------------------------------

static std::mutex                          s_catalog_mu;
static std::map<std::string, uint32_t>     s_name_to_id;
static std::map<uint32_t, TableMeta>       s_id_to_meta;
static uint64_t                            s_next_table_id = 1;

// Rebuilds the in-memory caches by scanning the catalog key range and reading
// the counter key.  Called once during plugin init.
static bool catalog_init(bytecask_db_t *db) {
  // Read the current table_id counter.
  {
    auto ckey = counter_key(kCounterTableId);
    uint8_t *val = nullptr;
    std::size_t val_len = 0;
    int rc = bytecask_get(db, ckey.data(), ckey.size(), &val, &val_len);
    if (rc == 1) {
      s_next_table_id = decode_counter_value(val, val_len) + 1;
      bytecask_free_buf(val);
    } else if (rc == 0) {
      s_next_table_id = 1;
    } else {
      fprintf(stderr, "[ha_bytecaskdb] Failed to read table_id counter: %s\n",
              bytecask_errmsg());
      return false;
    }
  }

  // Scan all table metadata entries.
  auto [lower, upper] = table_meta_scan_bounds();
  auto *iter = bytecask_iter_open(db, lower.data(), lower.size());
  if (!iter) {
    fprintf(stderr, "[ha_bytecaskdb] Failed to open catalog iterator: %s\n",
            bytecask_errmsg());
    return false;
  }

  while (bytecask_iter_valid(iter)) {
    uint8_t *key = nullptr;
    std::size_t key_len = 0;
    if (bytecask_iter_key(iter, &key, &key_len) != 0) {
      break;
    }

    // Stop if we've passed the upper bound.
    if (key_len < upper.size() ||
        std::memcmp(key, upper.data(), upper.size()) >= 0) {
      bytecask_free_buf(key);
      break;
    }

    uint8_t *val = nullptr;
    std::size_t val_len = 0;
    if (bytecask_iter_value(iter, &val, &val_len) != 0) {
      bytecask_free_buf(key);
      break;
    }

    TableMeta meta;
    if (deserialize_table_meta(val, val_len, meta)) {
      s_name_to_id.emplace(meta.full_name, meta.table_id);
      s_id_to_meta.emplace(meta.table_id, std::move(meta));
    }

    bytecask_free_buf(key);
    bytecask_free_buf(val);
    bytecask_iter_next(iter);
  }

  bytecask_iter_free(iter);

  fprintf(stderr, "[ha_bytecaskdb] Catalog loaded: %zu tables, next_id=%lu\n",
          s_name_to_id.size(), static_cast<unsigned long>(s_next_table_id));
  return true;
}

// Allocates a new table_id atomically using a snapshot + write plan with
// ensure_unchanged on the counter key.  Retries on conflict.
uint32_t catalog_alloc_table_id(bytecask_db_t *db) {
  auto ckey = counter_key(kCounterTableId);

  for (;;) {
    auto *snap = bytecask_snapshot(db);
    if (!snap) {
      return 0;
    }

    // Read current counter from snapshot.
    uint8_t *val = nullptr;
    std::size_t val_len = 0;
    uint64_t current_id = 0;
    int rc = bytecask_snapshot_get(snap, ckey.data(), ckey.size(),
                                   &val, &val_len);
    if (rc == 1) {
      current_id = decode_counter_value(val, val_len);
      bytecask_free_buf(val);
    } else if (rc < 0) {
      bytecask_snapshot_free(snap);
      return 0;
    }

    uint64_t new_id = current_id + 1;
    auto new_val = encode_counter_value(new_id);

    auto *plan = bytecask_write_plan_new_with_snapshot(snap);
    // snap is consumed — don't free it.
    bytecask_write_plan_put(plan, ckey.data(), ckey.size(),
                            new_val.data(), new_val.size());

    int result = bytecask_apply_batch(db, plan, /*sync=*/1);
    if (result == 1) {
      // Committed.
      std::lock_guard<std::mutex> lk{s_catalog_mu};
      s_next_table_id = new_id + 1;
      return static_cast<uint32_t>(new_id);
    }
    if (result < 0) {
      return 0;
    }
    // result == 0: conflict, retry.
  }
}

// Persists table metadata and updates in-memory caches.
bool catalog_put_table_meta(bytecask_db_t *db, const TableMeta &meta,
                            const char *name) {
  auto normalized = normalize_table_name(name);
  auto key = table_meta_key(name);
  auto val = serialize_table_meta(meta);

  int rc = bytecask_put(db, key.data(), key.size(),
                        val.data(), val.size(), /*sync=*/1);
  if (rc != 0) {
    return false;
  }

  std::lock_guard<std::mutex> lk{s_catalog_mu};
  s_name_to_id[normalized] = meta.table_id;
  s_id_to_meta[meta.table_id] = meta;
  return true;
}

// Deletes table metadata and updates in-memory caches.
bool catalog_delete_table_meta(bytecask_db_t *db, const char *name) {
  auto normalized = normalize_table_name(name);
  auto key = table_meta_key(name);

  int rc = bytecask_del(db, key.data(), key.size(), /*sync=*/1);
  if (rc < 0) {
    return false;
  }

  std::lock_guard<std::mutex> lk{s_catalog_mu};
  auto it = s_name_to_id.find(normalized);
  if (it != s_name_to_id.end()) {
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
    s_id_to_meta.erase(it->second);
    s_name_to_id.erase(it);
  }
}

// Atomically renames table metadata: deletes old key, inserts new key.
bool catalog_rename_table_meta(bytecask_db_t *db,
                               const char *from, const char *to) {
  auto old_normalized = normalize_table_name(from);
  auto new_normalized = normalize_table_name(to);
  auto old_key = table_meta_key(from);
  auto new_key = table_meta_key(to);

  // Read the existing metadata.
  uint8_t *val = nullptr;
  std::size_t val_len = 0;
  int rc = bytecask_get(db, old_key.data(), old_key.size(), &val, &val_len);
  if (rc != 1) {
    return false;
  }

  // Deserialize, update the full_name, re-serialize.
  TableMeta meta;
  if (!deserialize_table_meta(val, val_len, meta)) {
    bytecask_free_buf(val);
    return false;
  }
  bytecask_free_buf(val);

  meta.full_name = new_normalized;
  auto new_val = serialize_table_meta(meta);

  // Atomic: delete old + put new.
  auto *plan = bytecask_write_plan_new();
  bytecask_write_plan_del(plan, old_key.data(), old_key.size());
  bytecask_write_plan_put(plan, new_key.data(), new_key.size(),
                          new_val.data(), new_val.size());
  int result = bytecask_apply_batch(db, plan, /*sync=*/1);
  if (result != 1) {
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

// ---------------------------------------------------------------------------
// Plugin init / deinit
// ---------------------------------------------------------------------------

static int bytecaskdb_init(void *p) {
  auto *hton = static_cast<handlerton *>(p);
  bytecaskdb_hton = hton;

  hton->create           = bytecaskdb_create_handler;
  hton->commit           = bytecaskdb_commit;
  hton->rollback         = bytecaskdb_rollback;
  hton->close_connection = bytecaskdb_close_connection;
  hton->flags            = HTON_NO_FLAGS;

  // Open the global database inside MariaDB's data directory.
  std::string db_path = std::string(mysql_real_data_home) + "bytecaskdb";

  g_db = bytecask_open(db_path.c_str(), /*recovery_threads=*/4);
  if (!g_db) {
    const char *err = bytecask_errmsg();
    fprintf(stderr, "[ha_bytecaskdb] Failed to open global DB at '%s': %s\n",
            db_path.c_str(), err ? err : "unknown error");
    return 1;
  }

  if (!catalog_init(g_db)) {
    fprintf(stderr, "[ha_bytecaskdb] Failed to initialize catalog\n");
    bytecask_close(g_db);
    g_db = nullptr;
    return 1;
  }

  fprintf(stderr, "[ha_bytecaskdb] Opened global DB at '%s'\n",
          db_path.c_str());
  return 0;
}

static int bytecaskdb_deinit(void * /*p*/) {
  if (g_db) {
    bytecask_close(g_db);
    g_db = nullptr;
  }
  // Clear caches.
  std::lock_guard<std::mutex> lk{s_catalog_mu};
  s_name_to_id.clear();
  s_id_to_meta.clear();
  s_next_table_id = 1;
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
