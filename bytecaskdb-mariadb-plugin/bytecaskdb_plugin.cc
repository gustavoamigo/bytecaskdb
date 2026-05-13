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
#include "log.h"

#include "ha_bytecaskdb.h"
#include "catalog.h"
#include "bytecaskdb_txn.h"
#include "bytecask_view.h"
#include "key_encoding.h"

#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#ifndef PLUGIN_TESTING
// ---------------------------------------------------------------------------
// System variables (global scope — MariaDB plugin API requirement)
// ---------------------------------------------------------------------------

static my_bool sysvar_use_mmap = FALSE;
static MYSQL_SYSVAR_BOOL(use_mmap, sysvar_use_mmap,
    PLUGIN_VAR_READONLY,
    "Use mmap for sealed data files (default OFF)",
    nullptr, nullptr, FALSE);

static struct st_mysql_sys_var *bytecaskdb_system_variables[] = {
    MYSQL_SYSVAR(use_mmap),
    nullptr,
};
#endif // !PLUGIN_TESTING

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

std::atomic<uint64_t> *catalog_autoinc_ptr(uint32_t table_id) {
  return &autoinc_counter_for(table_id);
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

// ---------------------------------------------------------------------------
// Per-table row count counters — seeded at startup, maintained by handler.
// ---------------------------------------------------------------------------

static std::mutex                                       s_row_count_mu;
static std::map<uint32_t, std::unique_ptr<std::atomic<int64_t>>>
                                                        s_row_counts;

static std::atomic<int64_t> &row_count_for(uint32_t table_id) {
  std::lock_guard<std::mutex> lk{s_row_count_mu};
  auto it = s_row_counts.find(table_id);
  if (it == s_row_counts.end()) {
    auto [ins, _] = s_row_counts.emplace(
        table_id, std::make_unique<std::atomic<int64_t>>(0));
    it = ins;
  }
  return *it->second;
}

int64_t catalog_row_count(uint32_t table_id) {
  return row_count_for(table_id).load();
}

std::atomic<int64_t> *catalog_row_count_ptr(uint32_t table_id) {
  return &row_count_for(table_id);
}

void catalog_row_count_add(uint32_t table_id, int64_t delta) {
  row_count_for(table_id).fetch_add(delta);
}

void catalog_row_count_reset(uint32_t table_id) {
  row_count_for(table_id).store(0);
}

void catalog_drop_row_count(uint32_t table_id) {
  std::lock_guard<std::mutex> lk{s_row_count_mu};
  s_row_counts.erase(table_id);
}

// Rebuilds the in-memory caches by scanning the catalog key range. Called
// once during plugin init.
static bool catalog_init(bytecask::DB *db) {
  auto [lower, upper] = table_meta_scan_bounds();
  try {
    for (auto &entry : db->iter_from({}, as_view(lower.data(), lower.size()))) {
      // Stop if past the upper bound.
      const auto *kp = u8_data(entry.key);
      if (entry.key.size() < upper.size() ||
          std::memcmp(kp, upper.data(), upper.size()) >= 0) {
        break;
      }

      TableMeta meta;
      if (deserialize_table_meta(u8_data(entry.value), entry.value.size(), meta)) {
        s_name_to_id.emplace(meta.full_name, meta.table_id);
        s_id_to_meta.emplace(meta.table_id, std::move(meta));
      }
    }
  } catch (const std::exception &e) {
    sql_print_error("ByteCaskDB: catalog scan failed: %s", e.what());
    return false;
  }

  sql_print_information("ByteCaskDB: catalog loaded: %zu tables",
          s_name_to_id.size());

  // Seed per-table row counts by scanning the row namespace for each table.
  for (auto &[tid, meta] : s_id_to_meta) {
    auto lo = table_id_prefix(tid);
    auto hi = table_id_upper_bound(tid);
    int64_t count = 0;
    try {
      for (auto &k : db->keys_from({}, as_view(lo.data(), lo.size()))) {
        if (k.size() < hi.size() ||
            std::memcmp(u8_data(k), hi.data(), hi.size()) >= 0) {
          break;
        }
        ++count;
      }
    } catch (const std::exception &) {
    }
    row_count_for(tid).store(count);
  }

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
      sql_print_error("ByteCaskDB: alloc_table_id read failed: %s",
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
      sql_print_error("ByteCaskDB: alloc_table_id apply failed: %s",
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
    sql_print_error("ByteCaskDB: put_table_meta failed: %s", e.what());
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
    sql_print_error("ByteCaskDB: delete_table_meta failed: %s",
            e.what());
    return false;
  }

  std::lock_guard<std::mutex> lk{s_catalog_mu};
  auto it = s_name_to_id.find(normalized);
  if (it != s_name_to_id.end()) {
    catalog_drop_rowid(it->second);
    catalog_drop_autoinc(it->second);
    catalog_drop_row_count(it->second);
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
    catalog_drop_row_count(it->second);
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
    sql_print_error("ByteCaskDB: rename_table_meta read failed: %s",
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
    sql_print_error("ByteCaskDB: rename_table_meta apply failed: %s",
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

} // namespace bytecaskdb

#ifndef PLUGIN_TESTING

namespace bytecaskdb {

// ---------------------------------------------------------------------------
// Handler factory — called by MariaDB for every open/create
// ---------------------------------------------------------------------------

static handler *bytecaskdb_create_handler(handlerton *hton,
                                          TABLE_SHARE *table,
                                          MEM_ROOT *mem_root) {
  return new (mem_root) ha_bytecaskdb(hton, table);
}

// ---------------------------------------------------------------------------
// Handlerton callbacks — commit / rollback / savepoints / close_connection
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

static int bytecaskdb_savepoint_set(handlerton *hton, THD *thd, void *sv) {
  auto *txn = static_cast<MariaDBTxn *>(thd_get_ha_data(thd, hton));
  if (!txn) { return 0; }
  txn->savepoint_set(sv);
  return 0;
}

static int bytecaskdb_savepoint_rollback(handlerton *hton, THD *thd, void *sv) {
  auto *txn = static_cast<MariaDBTxn *>(thd_get_ha_data(thd, hton));
  if (!txn) { return 0; }
  txn->savepoint_rollback(sv);
  return 0;
}

static int bytecaskdb_savepoint_release(handlerton *hton, THD *thd, void *sv) {
  auto *txn = static_cast<MariaDBTxn *>(thd_get_ha_data(thd, hton));
  if (!txn) { return 0; }
  txn->savepoint_release(sv);
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
// SHOW ENGINE BYTECASKDB STATUS
// ---------------------------------------------------------------------------

static bool bytecaskdb_show_status(handlerton * /*hton*/, THD *thd,
                                   stat_print_fn *stat_print,
                                   enum ha_stat_type stat_type) {
  if (stat_type != HA_ENGINE_STATUS)
    return false;

  auto counters = g_db->stats();
  std::string output;
  for (auto &[name, value] : counters)
    output += name + ": " + std::to_string(value) + "\n";

  if (g_db->is_degraded())
    output += "degraded_reason: " + g_db->degraded_reason() + "\n";

  return stat_print(thd, "BYTECASKDB", 10, "", 0,
                    output.c_str(), output.size());
}

// ---------------------------------------------------------------------------
// Background vacuum thread
// ---------------------------------------------------------------------------

static std::thread             s_vacuum_thread;
static std::mutex              s_vacuum_mu;
static std::condition_variable s_vacuum_cv;
static bool                    s_vacuum_stop = false;

// Backup: vacuum pause state (guarded by s_vacuum_mu).
static int                                s_vacuum_pause_count = 0;
static bool                               s_vacuum_in_progress = false;
static std::condition_variable            s_vacuum_idle_cv;
static std::optional<bytecask::FileManifest> s_backup_manifest;

static void vacuum_loop() {
  static constexpr auto kBusyInterval = std::chrono::milliseconds{500};
  static constexpr auto kIdleInterval = std::chrono::seconds{30};

  std::unique_lock<std::mutex> lk{s_vacuum_mu};
  while (!s_vacuum_stop) {
    bool more_work = false;
    if (g_db && s_vacuum_pause_count == 0) {
      s_vacuum_in_progress = true;
      lk.unlock();
      try {
        more_work = g_db->vacuum();
      } catch (const std::exception &e) {
        sql_print_error("ByteCaskDB: vacuum error: %s", e.what());
      }
      lk.lock();
      s_vacuum_in_progress = false;
      s_vacuum_idle_cv.notify_all();
    }
    s_vacuum_cv.wait_for(lk, more_work ? kBusyInterval : kIdleInterval,
                         [] { return s_vacuum_stop; });
  }
}

// ---------------------------------------------------------------------------
// Backup: manifest file helpers
// ---------------------------------------------------------------------------

static constexpr const char *kBackupManifestName = "backup_manifest.txt";

static void write_backup_manifest(const std::string &db_path,
                                  const bytecask::FileManifest &manifest) {
  auto path = std::filesystem::path{db_path} / kBackupManifestName;
  std::ofstream out{path, std::ios::trunc};
  out << "# ByteCaskDB backup manifest\n";
  out << "# through_sequence: " << manifest.through_sequence << "\n";
  for (const auto &f : manifest.files) {
    out << f.data_path.filename().string() << "\n";
    out << f.hint_path.filename().string() << "\n";
  }
  out.flush();
}

static void remove_backup_manifest(const std::string &db_path) {
  std::filesystem::remove(std::filesystem::path{db_path} / kBackupManifestName);
}

// Called during bytecaskdb_init before DB::open(). If backup_manifest.txt
// exists, this is a restore from backup: move any .data/.hint files not
// listed in the manifest to a discarded/ subfolder, then delete the manifest.
static void apply_backup_manifest(const std::string &db_path) {
  auto manifest_path = std::filesystem::path{db_path} / kBackupManifestName;
  if (!std::filesystem::exists(manifest_path))
    return;

  std::set<std::string> allowed;
  {
    std::ifstream in{manifest_path};
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      allowed.insert(line);
    }
  }

  auto discard_dir = std::filesystem::path{db_path} / "discarded";
  bool discarded_any = false;
  for (const auto &entry : std::filesystem::directory_iterator{db_path}) {
    if (!entry.is_regular_file()) continue;
    auto name = entry.path().filename().string();
    auto ext = entry.path().extension().string();
    if (ext != ".data" && ext != ".hint") continue;
    if (allowed.contains(name)) continue;

    if (!discarded_any) {
      std::filesystem::create_directories(discard_dir);
      discarded_any = true;
    }
    std::filesystem::rename(entry.path(), discard_dir / name);
    sql_print_information("ByteCaskDB: restore — moved unlisted file '%s' "
                          "to discarded/", name.c_str());
  }

  std::filesystem::remove(manifest_path);
  sql_print_information("ByteCaskDB: restore — backup manifest applied, "
                        "%zu files retained", allowed.size());
}

// ---------------------------------------------------------------------------
// Backup: handlerton callbacks
// ---------------------------------------------------------------------------

static void bytecaskdb_prepare_for_backup() {
  if (!g_db) return;

  std::unique_lock<std::mutex> lk{s_vacuum_mu};
  s_vacuum_pause_count++;
  s_vacuum_idle_cv.wait(lk, [] { return !s_vacuum_in_progress; });

  lk.unlock();
  auto manifest = g_db->create_manifest();
  std::string db_path = std::string(mysql_real_data_home) + "bytecaskdb";
  write_backup_manifest(db_path, manifest);
  lk.lock();
  s_backup_manifest.emplace(std::move(manifest));

  sql_print_information("ByteCaskDB: backup started — vacuum paused, "
                        "%zu files sealed, through_sequence=%" PRIu64,
                        s_backup_manifest->files.size(),
                        s_backup_manifest->through_sequence);
}

static void bytecaskdb_end_backup() {
  std::string db_path = std::string(mysql_real_data_home) + "bytecaskdb";

  std::lock_guard<std::mutex> lk{s_vacuum_mu};
  s_backup_manifest.reset();
  remove_backup_manifest(db_path);
  if (s_vacuum_pause_count > 0)
    s_vacuum_pause_count--;
  s_vacuum_cv.notify_one();

  sql_print_information("ByteCaskDB: backup ended — vacuum resumed");
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
  hton->show_status              = bytecaskdb_show_status;
  hton->flags                    = HTON_SUPPORTS_FOREIGN_KEYS;
  hton->savepoint_offset         = sizeof(uint32_t);
  hton->savepoint_set            = bytecaskdb_savepoint_set;
  hton->savepoint_rollback       = bytecaskdb_savepoint_rollback;
  hton->savepoint_release        = bytecaskdb_savepoint_release;
  hton->prepare_for_backup       = bytecaskdb_prepare_for_backup;
  hton->end_backup               = bytecaskdb_end_backup;

  // Open the global database inside MariaDB's data directory.
  std::string db_path = std::string(mysql_real_data_home) + "bytecaskdb";

  // If backup_manifest.txt exists, this is a restore from backup: move
  // unlisted files to discarded/ before opening.
  apply_backup_manifest(db_path);

  bytecask::Options opts;
  opts.recovery_threads = 4;
  opts.max_value_bytes = 16 * 1024 * 1024;  // MEDIUMBLOB (16 MiB)
  opts.max_key_bytes = 8192;  // secondary index key + PK suffix can exceed 4096
  opts.use_mmap = sysvar_use_mmap;

  try {
    g_db_owner = std::make_unique<DBHolder>(db_path, opts);
    g_db = &g_db_owner->db;
  } catch (const std::exception &e) {
    sql_print_error("ByteCaskDB: failed to open global DB at '%s': %s",
            db_path.c_str(), e.what());
    return 1;
  }

  if (!catalog_init(g_db)) {
    sql_print_error("ByteCaskDB: failed to initialize catalog");
    g_db = nullptr;
    g_db_owner.reset();
    return 1;
  }

  sql_print_information("ByteCaskDB: opened global DB at '%s'",
          db_path.c_str());
  sql_print_information("ByteCaskDB: use_mmap=%s",
          sysvar_use_mmap ? "ON" : "OFF");

  s_vacuum_stop = false;
  s_vacuum_pause_count = 0;
  s_vacuum_in_progress = false;
  s_backup_manifest.reset();
  s_vacuum_thread = std::thread{vacuum_loop};

  return 0;
}

static int bytecaskdb_deinit(void * /*p*/) {
  {
    std::lock_guard<std::mutex> lk{s_vacuum_mu};
    s_vacuum_stop = true;
  }
  s_vacuum_cv.notify_one();
  if (s_vacuum_thread.joinable())
    s_vacuum_thread.join();

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
  {
    std::lock_guard<std::mutex> lk{s_row_count_mu};
    s_row_counts.clear();
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
    bytecaskdb_system_variables,
    "0.2",            // version string
    MariaDB_PLUGIN_MATURITY_GAMMA,
} maria_declare_plugin_end;

#endif // !PLUGIN_TESTING