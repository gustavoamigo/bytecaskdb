// ha_bytecaskdb.cc — MariaDB handler implementation for ByteCaskDB (Phase 1).
//
// Implements: CREATE TABLE, INSERT, SELECT *, DROP TABLE.
// Not implemented (Phase 1): UPDATE, DELETE, secondary indexes, transactions.

#include "ha_bytecaskdb.h"
#include "table.h"   // full TABLE / TABLE_SHARE definitions
#include "bytecask_c.h"
#include "key_encoding.h"
#include "row_encoding.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace bytecaskdb {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ha_bytecaskdb::ha_bytecaskdb(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {}

// ---------------------------------------------------------------------------
// Lock management — no-op for Phase 1 (no transactions, no MVCC yet)
// ---------------------------------------------------------------------------

THR_LOCK_DATA **ha_bytecaskdb::store_lock(THD * /*thd*/, THR_LOCK_DATA **to,
                                          enum thr_lock_type /*lock_type*/) {
  // Return 'to' unchanged — we do not participate in MariaDB's table locking.
  return to;
}

// ---------------------------------------------------------------------------
// create() — called for CREATE TABLE
// ---------------------------------------------------------------------------

int ha_bytecaskdb::create(const char *name, TABLE * /*table_arg*/,
                           HA_CREATE_INFO * /*create_info*/) {
  // Assign a table_id.  The global DB is already open; no per-table files.
  (void)get_or_assign_table_id(name);
  return 0;
}

// ---------------------------------------------------------------------------
// open() — called when a statement first opens the table
// ---------------------------------------------------------------------------

int ha_bytecaskdb::open(const char *name, int /*mode*/,
                         uint /*test_if_locked*/) {
  if (!g_db) {
    return HA_ERR_GENERIC;
  }
  table_id_   = get_or_assign_table_id(name);
  scan_iter_  = nullptr;
  ref_length  = 4 + table->key_info[table->s->primary_key].key_length;
  return 0;
}

// ---------------------------------------------------------------------------
// close() — called when a statement is done with the table
// ---------------------------------------------------------------------------

int ha_bytecaskdb::close() {
  if (scan_iter_) {
    bytecask_iter_free(scan_iter_);
    scan_iter_ = nullptr;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// delete_table() — called for DROP TABLE
// ---------------------------------------------------------------------------

int ha_bytecaskdb::delete_table(const char *name) {
  // There are no per-table files — the data lives in the global DB under
  // the table's key prefix.  Remove the table_id assignment from memory.
  //
  // Phase 2 will add: scan and delete all keys with this table's prefix.
  // For Phase 1 we accept that old keys linger until the server restarts
  // (the table_id is not re-assigned after restart today).
  remove_table_id(name);
  return 0;
}

// ---------------------------------------------------------------------------
// write_row() — called for each INSERT row
// ---------------------------------------------------------------------------

int ha_bytecaskdb::write_row(const uchar *buf) {
  if (!g_db) { return HA_ERR_GENERIC; }

  auto key = encode_pk(buf);
  auto val = encode_row(table, buf);

  // Use sync=1 (durable write) by default.  Phase 3 will route this through
  // the transaction layer with configurable durability.
  int rc = bytecask_put(g_db,
                         key.data(), key.size(),
                         val.data(), val.size(),
                         /*sync=*/1);
  if (rc != 0) {
    const char *err = bytecask_errmsg();
    fprintf(stderr, "[ha_bytecaskdb] write_row failed: %s\n",
            err ? err : "unknown");
    return HA_ERR_GENERIC;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// rnd_init() / rnd_next() / rnd_end() — full table scan
// ---------------------------------------------------------------------------

int ha_bytecaskdb::rnd_init(bool /*scan*/) {
  if (!g_db) { return HA_ERR_GENERIC; }

  // Position the iterator at the start of this table's key range.
  auto prefix = table_id_prefix(table_id_);
  scan_iter_ = bytecask_iter_open(g_db,
                                   prefix.data(), prefix.size());
  if (!scan_iter_) {
    const char *err = bytecask_errmsg();
    fprintf(stderr, "[ha_bytecaskdb] rnd_init: iter_open failed: %s\n",
            err ? err : "unknown");
    return HA_ERR_GENERIC;
  }
  return 0;
}

int ha_bytecaskdb::rnd_next(uchar *buf) {
  if (!scan_iter_) { return HA_ERR_END_OF_FILE; }

  // Skip entries that don't belong to this table (different table_id prefix).
  while (bytecask_iter_valid(scan_iter_)) {
    uint8_t *key_buf = nullptr;
    std::size_t key_len = 0;

    if (bytecask_iter_key(scan_iter_, &key_buf, &key_len) != 0) {
      bytecask_free_buf(key_buf);
      return HA_ERR_GENERIC;
    }

    bool belongs = key_belongs_to_table(key_buf, key_len, table_id_);
    bytecask_free_buf(key_buf);

    if (!belongs) {
      // Past this table's prefix — scan complete.
      return HA_ERR_END_OF_FILE;
    }

    // Read the value and decode into the record buffer.
    uint8_t *val_buf = nullptr;
    std::size_t val_len = 0;

    if (bytecask_iter_value(scan_iter_, &val_buf, &val_len) != 0) {
      bytecask_free_buf(val_buf);
      return HA_ERR_GENERIC;
    }

    decode_row(table, val_buf, val_len, buf);
    bytecask_free_buf(val_buf);

    // Advance past this entry for the next call.
    bytecask_iter_next(scan_iter_);

    // Unpack variable-length fields (VARCHAR etc.) from the row buffer so
    // MariaDB can return them correctly to the client.
    memset(buf, 0, table->s->null_bytes);

    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int ha_bytecaskdb::rnd_end() {
  if (scan_iter_) {
    bytecask_iter_free(scan_iter_);
    scan_iter_ = nullptr;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// position() / rnd_pos() — stable row references for UPDATE / DELETE (Phase 2)
// ---------------------------------------------------------------------------

void ha_bytecaskdb::position(const uchar *record) {
  // Store the encoded PK in `ref` (a buffer of `ref_length` bytes allocated
  // by MariaDB and available as a member of handler).
  auto pk = encode_pk(record);
  assert(pk.size() <= ref_length);
  std::memcpy(ref, pk.data(), pk.size());
}

int ha_bytecaskdb::rnd_pos(uchar *buf, uchar *pos) {
  if (!g_db) { return HA_ERR_GENERIC; }

  uint8_t *val_buf = nullptr;
  std::size_t val_len = 0;

  int found = bytecask_get(g_db, pos, ref_length, &val_buf, &val_len);
  if (found < 0) {
    const char *err = bytecask_errmsg();
    fprintf(stderr, "[ha_bytecaskdb] rnd_pos get failed: %s\n",
            err ? err : "unknown");
    return HA_ERR_GENERIC;
  }
  if (found == 0) {
    return HA_ERR_KEY_NOT_FOUND;
  }

  decode_row(table, val_buf, val_len, buf);
  bytecask_free_buf(val_buf);
  return 0;
}

// ---------------------------------------------------------------------------
// info() — optimizer statistics
// ---------------------------------------------------------------------------

int ha_bytecaskdb::info(uint /*flag*/) {
  // Return HA_POS_ERROR to signal "unknown row count".
  // The optimizer still works with this; it just uses worst-case estimates.
  stats.records = HA_POS_ERROR;
  return 0;
}

// ---------------------------------------------------------------------------
// encode_pk() — private helper
// ---------------------------------------------------------------------------

std::vector<uint8_t> ha_bytecaskdb::encode_pk(const uchar *buf) const {
  return bytecaskdb::encode_pk(table, buf, table_id_);
}

} // namespace bytecaskdb
