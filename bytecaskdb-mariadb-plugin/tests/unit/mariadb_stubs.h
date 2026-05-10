// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Gustavo Amigo
//
// mariadb_stubs.h — Minimal type stubs for unit-testing MariaDB plugin
// encoding functions without linking against MariaDB server headers.
//
// Provides the subset of types that row_encoding.cc, key_encoding.cc,
// bytecaskdb_txn.cc, and ha_bytecaskdb.cc access when compiled with
// PLUGIN_TESTING or BYTECASKDB_TESTS.

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <atomic>

// MariaDB type aliases.
typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long long ulonglong;
typedef unsigned long ulong;
typedef uint64_t key_part_map;
typedef uint64_t ha_rows;

#define MAX_KEY 64
#define MAX_KEY_LENGTH 3072

// MariaDB key flags.
#define HA_NOSAME 1
#define HA_BLOB_PART 4
#define HA_KEY_BLOB_LENGTH 2

// MariaDB error codes used by the handler.
#define HA_ERR_GENERIC        -1
#define HA_ERR_END_OF_FILE    137
#define HA_ERR_KEY_NOT_FOUND  120
#define HA_ERR_FOUND_DUPP_KEY 121
#define HA_ERR_LOCK_DEADLOCK  1213
#define HA_ERR_INTERNAL_ERROR 122
#define HA_ERR_TABLE_DEF_CHANGED 1412
#define HA_ERR_TABLE_EXIST    156
#define HA_ERR_NO_SUCH_TABLE  157

// File lock constants.
#define F_UNLCK  2
#define F_RDLCK  0
#define F_WRLCK  1

// MariaDB handler info flags.
#define HA_STATUS_VARIABLE    1
#define HA_STATUS_AUTO        16
#define HA_STATUS_ERRKEY      128

// MariaDB index capability flags.
#define HA_READ_NEXT     1UL
#define HA_READ_PREV     2UL
#define HA_READ_ORDER    4UL
#define HA_READ_RANGE    8UL
#define HA_KEYREAD_ONLY  16UL

// MariaDB ALTER flags.
#define ALTER_DROP_FOREIGN_KEY  0x01ULL
#define ALTER_COLUMN_NAME      0x02ULL

// MariaDB table flags.
#define HA_REC_NOT_IN_SEQ         (1ULL << 0)
#define HA_BINLOG_ROW_CAPABLE     (1ULL << 1)
#define HA_NULL_IN_KEY            (1ULL << 2)
#define HA_CAN_INDEX_BLOBS        (1ULL << 3)
#define HA_AUTO_PART_KEY          (1ULL << 4)
#define HA_CAN_VIRTUAL_COLUMNS    (1ULL << 5)
#define HA_PRIMARY_KEY_IN_READ_INDEX (1ULL << 6)

// MariaDB admin return codes.
#define HA_ADMIN_OK       0
#define HA_ADMIN_FAILED   -1
#define HA_ADMIN_CORRUPT  -2

// MariaDB CREATE_INFO flags.
#define HA_CREATE_USED_AUTO  0x0001

// MariaDB extra operations.
enum ha_extra_function {
  HA_EXTRA_NO_READCHECK = 0,
  HA_EXTRA_KEYREAD = 1,
  HA_EXTRA_NO_KEYREAD = 2,
};

// MariaDB key read functions.
enum ha_rkey_function {
  HA_READ_KEY_EXACT = 0,
  HA_READ_KEY_OR_NEXT = 1,
  HA_READ_KEY_OR_PREV = 2,
  HA_READ_AFTER_KEY = 3,
  HA_READ_BEFORE_KEY = 4,
  HA_READ_PREFIX = 5,
  HA_READ_PREFIX_LAST = 6,
  HA_READ_PREFIX_LAST_OR_PREV = 7,
};

// MariaDB ALTER TABLE support.
enum enum_alter_inplace_result {
  HA_ALTER_ERROR = 0,
  HA_ALTER_INPLACE_NOT_SUPPORTED = 1,
  HA_ALTER_INPLACE_EXCLUSIVE_LOCK = 2,
  HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE = 3,
  HA_ALTER_INPLACE_SHARED_LOCK = 4,
  HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE = 5,
  HA_ALTER_INPLACE_NO_LOCK = 6,
};

// MariaDB lock types.
enum thr_lock_type {
  TL_UNLOCK = 0,
  TL_READ = 1,
  TL_WRITE = 2,
};

// MariaDB field type enum (subset).
enum enum_field_types {
  MYSQL_TYPE_DECIMAL,
  MYSQL_TYPE_TINY,
  MYSQL_TYPE_SHORT,
  MYSQL_TYPE_LONG,
  MYSQL_TYPE_FLOAT,
  MYSQL_TYPE_DOUBLE,
  MYSQL_TYPE_NULL,
  MYSQL_TYPE_TIMESTAMP,
  MYSQL_TYPE_LONGLONG,
  MYSQL_TYPE_INT24,
  MYSQL_TYPE_DATE,
  MYSQL_TYPE_TIME,
  MYSQL_TYPE_DATETIME,
  MYSQL_TYPE_YEAR,
  MYSQL_TYPE_NEWDATE,
  MYSQL_TYPE_VARCHAR,
  MYSQL_TYPE_BIT,
  MYSQL_TYPE_TIMESTAMP2 = 17,
  MYSQL_TYPE_DATETIME2 = 18,
  MYSQL_TYPE_TIME2 = 19,
  MYSQL_TYPE_NEWDECIMAL = 246,
  MYSQL_TYPE_ENUM = 247,
  MYSQL_TYPE_SET = 248,
  MYSQL_TYPE_TINY_BLOB = 249,
  MYSQL_TYPE_MEDIUM_BLOB = 250,
  MYSQL_TYPE_LONG_BLOB = 251,
  MYSQL_TYPE_BLOB = 252,
  MYSQL_TYPE_VAR_STRING = 253,
  MYSQL_TYPE_STRING = 254,
  MYSQL_TYPE_GEOMETRY = 255,
};

// MariaDB KEY_PART_INFO — describes one column within a KEY.
struct Field;
struct KEY_PART_INFO {
  Field    *field{nullptr};
  uint16_t  fieldnr{0};        // 1-based
  uint16_t  field_index{0};
  uint      null_bit{0};
  uint      key_part_flag{0};  // HA_BLOB_PART etc.
  uint      store_length{0};
  uint      length{0};
};

struct KEY {
  uint key_length{0};
  uint user_defined_key_parts{0};
  uint flags{0};               // HA_NOSAME for unique indexes
  KEY_PART_INFO *key_part{nullptr};
};

struct TABLE_SHARE {
  uint primary_key{0};
  uint32_t reclength{0};
  uint16_t null_bytes{0};
  uint fields{0};
  uint keys{0};
  uint32_t rec_buff_length{0};
  uint next_number_keypart{0};
};

// Minimal CHARSET_INFO stub.
#define MY_CS_NOPAD 0x2000
struct CHARSET_INFO {
  uint number{0};
  uint state{0};
};

struct Field {
  uchar *ptr{nullptr};
  uint16_t field_index{0};
  uint field_length{0};

  virtual ~Field() = default;
  virtual enum_field_types type() const { return MYSQL_TYPE_LONG; }
  virtual enum_field_types real_type() const { return type(); }
  virtual bool is_null_in_record(const uchar *) const { return false; }
  virtual bool real_maybe_null() const { return false; }
  virtual bool is_unsigned() const { return false; }
  virtual const CHARSET_INFO *charset() const { return nullptr; }
  virtual int64_t val_int() const { return 0; }
  ptrdiff_t offset(const uchar *record) const {
    return ptr ? (ptr - record) : 0;
  }
};

struct Field_long : public Field {
  enum_field_types type() const override { return MYSQL_TYPE_LONG; }
  int64_t val_int() const override {
    if (!ptr) return 0;
    int32_t v;
    std::memcpy(&v, ptr, sizeof(v));
    return v;
  }
};

struct Field_blob : public Field {
  uint32_t pack_len_no_ptr_{4};

  enum_field_types type() const override { return MYSQL_TYPE_BLOB; }

  uint32_t get_length(const uchar *pos) const {
    uint32_t len = 0;
    std::memcpy(&len, pos, pack_len_no_ptr_);
    return len;
  }

  uint32_t pack_length_no_ptr() const { return pack_len_no_ptr_; }
};

// MariaDB key_range used by records_in_range.
struct key_range {
  const uchar *key{nullptr};
  uint length{0};
  key_part_map keypart_map{0};
  enum ha_rkey_function flag{HA_READ_KEY_EXACT};
};

// MariaDB page_range used by records_in_range.
struct page_range {
  double pages{0.0};
};

// Minimal MY_BITMAP stub.
struct MY_BITMAP {
  uint32_t *bitmap{nullptr};
  uint n_bits{0};
};
inline bool bitmap_is_set(const MY_BITMAP *map, uint bit) {
  if (!map || !map->bitmap) return true;
  return (map->bitmap[bit / 32] & (1U << (bit % 32))) != 0;
}

// Stub for THR_LOCK_DATA.
struct THR_LOCK_DATA {
  int dummy{0};
};

// Stub for HA_CREATE_INFO.
struct Alter_info;
struct HA_CREATE_INFO {
  ulonglong auto_increment_value{0};
  uint used_fields{0};
  Alter_info *alter_info{nullptr};
};

// Stub for HA_CHECK_OPT.
struct HA_CHECK_OPT {
  int dummy{0};
};

// Stub for Alter_inplace_info.
struct Alter_inplace_info {
  uint handler_flags{0};
};

// Stub List template — minimal linked-list placeholder.
template <typename T> struct List {
  int dummy{0};
};
struct FOREIGN_KEY_INFO {};

// Stub for Field with next_number_field tracking.
struct TABLE {
  TABLE_SHARE *s{nullptr};
  KEY *key_info{nullptr};
  Field **field{nullptr};
  uchar *record[2]{nullptr, nullptr};
  Field *next_number_field{nullptr};
  Field *found_next_number_field{nullptr};
  MY_BITMAP *write_set{nullptr};
  MY_BITMAP *read_set{nullptr};
};

// Transaction-related stubs for MariaDBTxn tests.
struct THD {
  void *ha_data[MAX_KEY]{};
  int dummy{0};
};

struct handlerton {
  int slot{0};
  int dummy{0};
  void (*prepare_for_backup)(){nullptr};
  void (*end_backup)(){nullptr};
};

// MariaDB session option flags.
#define OPTION_NOT_AUTOCOMMIT  0x00001ULL
#define OPTION_BEGIN           0x00002ULL

// Stub: thd_test_options — tests can set g_stub_thd_options to simulate modes.
inline uint64_t g_stub_thd_options = 0;
inline uint64_t thd_test_options(THD * /*thd*/, uint64_t options) {
  return g_stub_thd_options & options;
}

// Stub: trans_register_ha — no-op in tests.
inline void trans_register_ha(THD * /*thd*/, bool /*all*/,
                              handlerton * /*hton*/, int /*trx_id*/) {}

// Stub: thd_get_ha_data / thd_set_ha_data — use THD::ha_data[slot].
inline void *thd_get_ha_data(THD *thd, handlerton *hton) {
  return thd->ha_data[hton->slot];
}
inline void thd_set_ha_data(THD *thd, handlerton *hton, void *data) {
  thd->ha_data[hton->slot] = data;
}

// Stub: my_error / MYF — no-op in tests.
#define MYF(x) (x)
inline void my_error(int /*error*/, unsigned long /*flags*/, ...) {}

// MariaDB error codes.
#define ER_LOCK_DEADLOCK 1213

// Stubs for key_copy / key_restore — these are MariaDB server functions.
// For proof tests, key_copy must actually copy field data into the key buffer
// so that distinct PK values produce distinct encoded keys.
inline void key_copy(uchar *dst, const uchar *src,
                     KEY *key_info, uint key_length) {
  uint offset = 0;
  for (uint i = 0; i < key_info->user_defined_key_parts && offset < key_length; ++i) {
    auto &kp = key_info->key_part[i];
    uint len = kp.length;
    if (len > key_length - offset) len = key_length - offset;
    if (kp.field && kp.field->ptr) {
      std::memcpy(dst + offset, kp.field->ptr, len);
    }
    offset += kp.store_length;
  }
}
inline void key_restore(uchar *dst, const uchar *src,
                        KEY *key_info, uint key_length) {
  uint offset = 0;
  for (uint i = 0; i < key_info->user_defined_key_parts && offset < key_length; ++i) {
    auto &kp = key_info->key_part[i];
    uint len = kp.length;
    if (len > key_length - offset) len = key_length - offset;
    if (kp.field) {
      std::memcpy(kp.field->ptr, src + offset, len);
    }
    offset += kp.store_length;
  }
}

// Minimal handler base class stub.
class handler {
public:
  TABLE *table{nullptr};
  uint ref_length{sizeof(uint64_t)};
  uchar *ref{nullptr};
  uint active_index{MAX_KEY};
  uint errkey{0};

  struct ha_statistics {
    ha_rows records{0};
    ulonglong auto_increment_value{0};
  } stats;

  handler(handlerton * /*hton*/, TABLE_SHARE * /*share*/) {}
  virtual ~handler() = default;

  virtual int create(const char *, TABLE *, HA_CREATE_INFO *) { return 0; }
  virtual int open(const char *, int, uint) { return 0; }
  virtual int close() { return 0; }
  virtual int delete_table(const char *) { return 0; }
  virtual int rename_table(const char *, const char *) { return 0; }
  virtual int delete_all_rows() { return 0; }

  virtual int write_row(const uchar *) { return 0; }
  virtual int update_row(const uchar *, const uchar *) { return 0; }
  virtual int delete_row(const uchar *) { return 0; }

  virtual int rnd_init(bool) { return 0; }
  virtual int rnd_next(uchar *) { return HA_ERR_END_OF_FILE; }
  virtual int rnd_end() { return 0; }

  virtual int index_init(uint, bool) { return 0; }
  virtual int index_end() { return 0; }
  virtual int index_read_map(uchar *, const uchar *, key_part_map,
                             enum ha_rkey_function) { return HA_ERR_KEY_NOT_FOUND; }
  virtual int index_next(uchar *) { return HA_ERR_END_OF_FILE; }
  virtual int index_next_same(uchar *, const uchar *, uint) { return HA_ERR_END_OF_FILE; }
  virtual int index_prev(uchar *) { return HA_ERR_END_OF_FILE; }
  virtual int index_first(uchar *) { return HA_ERR_END_OF_FILE; }
  virtual int index_last(uchar *) { return HA_ERR_END_OF_FILE; }

  virtual int external_lock(THD *, int) { return 0; }

  virtual void get_auto_increment(ulonglong, ulonglong, ulonglong,
                                  ulonglong *, ulonglong *) {}
  virtual int reset_auto_increment(ulonglong) { return 0; }
  virtual void update_create_info(HA_CREATE_INFO *) {}

  virtual void position(const uchar *) {}
  virtual int rnd_pos(uchar *, uchar *) { return 0; }

  virtual ulonglong table_flags() const { return 0; }
  virtual const char *index_type(uint) { return ""; }
  virtual ulong index_flags(uint, uint, bool) const { return 0; }
  virtual int info(uint) { return 0; }
  virtual ha_rows records_in_range(uint, const key_range *, const key_range *,
                                   page_range *) { return 10; }

  virtual uint max_supported_key_length() const { return MAX_KEY_LENGTH; }
  virtual uint max_supported_key_part_length() const { return 3072; }
  virtual uint max_supported_keys() const { return MAX_KEY; }

  virtual THR_LOCK_DATA **store_lock(THD *, THR_LOCK_DATA **to,
                                     enum thr_lock_type) { return to; }
  virtual int extra(enum ha_extra_function) { return 0; }
  virtual int check(THD *, HA_CHECK_OPT *) { return 0; }

  virtual enum_alter_inplace_result check_if_supported_inplace_alter(
      TABLE *, Alter_inplace_info *) { return HA_ALTER_INPLACE_NOT_SUPPORTED; }
  virtual bool inplace_alter_table(TABLE *, Alter_inplace_info *) { return true; }
  virtual bool commit_inplace_alter_table(TABLE *, Alter_inplace_info *, bool) { return false; }
  virtual int get_foreign_key_list(THD *, List<FOREIGN_KEY_INFO> *) { return 0; }

  virtual int update_auto_increment() { return 0; }
};

// Stub: MariaDB logging functions.
inline void sql_print_information(const char * /*fmt*/, ...) {}
inline void sql_print_error(const char * /*fmt*/, ...) {}

// Stub: MariaDB global data directory.
inline const char *mysql_real_data_home = "/tmp/bytecaskdb_test/";
