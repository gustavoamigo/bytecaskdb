// bytecaskdb_plugin.cc — MariaDB plugin registration for ByteCaskDB.
//
// Implements the handlerton lifecycle:
//   bytecaskdb_init()   — opens the global DB at plugin-load time.
//   bytecaskdb_deinit() — closes the global DB at plugin-unload time.

#include "my_global.h"
#include "handler.h"
#include "mysql/plugin.h"

#include "ha_bytecaskdb.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace bytecaskdb {

// Definitions for the globals declared extern in ha_bytecaskdb.h.
bytecask_db_t *g_db           = nullptr;
handlerton    *bytecaskdb_hton = nullptr;

// ---------------------------------------------------------------------------
// Table-id registry
// Each table path is assigned a unique 32-bit identifier on first open.
// In Phase 1 this lives only in memory (not persisted across restarts).
// ---------------------------------------------------------------------------

static std::mutex                    s_table_id_mu;
static std::map<std::string, uint32_t> s_table_ids;
static uint32_t                      s_next_table_id = 1;

uint32_t get_or_assign_table_id(const std::string &table_path) {
  std::lock_guard<std::mutex> lk{s_table_id_mu};
  auto it = s_table_ids.find(table_path);
  if (it != s_table_ids.end()) {
    return it->second;
  }
  uint32_t id = s_next_table_id++;
  s_table_ids.emplace(table_path, id);
  return id;
}

void remove_table_id(const std::string &table_path) {
  std::lock_guard<std::mutex> lk{s_table_id_mu};
  s_table_ids.erase(table_path);
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
// Plugin init / deinit
// ---------------------------------------------------------------------------

static int bytecaskdb_init(void *p) {
  auto *hton = static_cast<handlerton *>(p);
  bytecaskdb_hton = hton;

  hton->create        = bytecaskdb_create_handler;
  hton->flags         = HTON_NO_FLAGS;
  // db_type is assigned by MariaDB; no need to set it here.

  // Open the global database inside MariaDB's data directory.
  // mysql_real_data_home is a MariaDB global exported from sql/mysqld.cc.
  // We use a sub-directory named "bytecaskdb" to keep all ByteCaskDB files
  // together.  Each table uses key prefixes to share this single DB.
  std::string db_path = std::string(mysql_real_data_home) + "bytecaskdb";

  g_db = bytecask_open(db_path.c_str(), /*recovery_threads=*/4);
  if (!g_db) {
    const char *err = bytecask_errmsg();
    fprintf(stderr, "[ha_bytecaskdb] Failed to open global DB at '%s': %s\n",
            db_path.c_str(), err ? err : "unknown error");
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
    "ByteCaskDB storage engine (Phase 1 POC)",
    PLUGIN_LICENSE_GPL,
    bytecaskdb::bytecaskdb_init,
    bytecaskdb::bytecaskdb_deinit,
    0x0001,           // version 0.1
    nullptr,          // status variables
    nullptr,          // system variables
    "0.1",            // version string
    MariaDB_PLUGIN_MATURITY_GAMMA,
} maria_declare_plugin_end;
