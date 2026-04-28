// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo
//
// bytecask_c.h — Stable C API for ByteCaskDB.
//
// This header provides a flat extern "C" interface over the C++23 module-based
// ByteCaskDB engine.  It is the only supported way to link against ByteCaskDB
// from out-of-tree code that cannot import C++23 modules directly (e.g. the
// MariaDB storage engine plugin).
//
// Ownership rules:
//   - bytecask_open() returns a heap-allocated opaque handle. The caller owns
//     it and must eventually call bytecask_close().
//   - bytecask_iter_*() return a heap-allocated opaque iterator. The caller
//     owns it and must eventually call bytecask_iter_free().
//   - bytecask_snapshot() returns a heap-allocated opaque snapshot. The caller
//     owns it and must eventually call bytecask_snapshot_free().
//   - bytecask_write_plan_new*() return a heap-allocated opaque write plan.
//     Plans are consumed by bytecask_apply_batch(); if not applied, free
//     with bytecask_write_plan_free().
//   - Value buffers written by get/iter functions are heap-allocated by the
//     callee and owned by the caller; free with bytecask_free_buf().
//
// Error convention: most functions return int (0 = success, non-zero = error).
// bytecask_errmsg() returns the last error string set on this thread.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Opaque handle types
// ---------------------------------------------------------------------------

typedef struct bytecask_db         bytecask_db_t;
typedef struct bytecask_iter       bytecask_iter_t;
typedef struct bytecask_snapshot   bytecask_snapshot_t;
typedef struct bytecask_write_plan bytecask_write_plan_t;

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------

// Opens (or creates) a ByteCaskDB at the filesystem path `dir`.
// `recovery_threads` — number of threads to use during key-directory recovery
// (pass 0 to use the default, currently 4).
// Returns NULL on failure; call bytecask_errmsg() for details.
bytecask_db_t *bytecask_open(const char *dir, unsigned recovery_threads);

// Closes the database and frees the handle.  Must not be called after a
// concurrent put/get/del/iter is still in flight.
void bytecask_close(bytecask_db_t *db);

// ---------------------------------------------------------------------------
// Write operations
// sync == 0: skip fdatasync (higher throughput, lower durability)
// sync != 0: fdatasync after write (default)
// ---------------------------------------------------------------------------

// Writes key → value.  Returns 0 on success, -1 on error.
int bytecask_put(bytecask_db_t *db,
                 const uint8_t *key,  size_t key_len,
                 const uint8_t *val,  size_t val_len,
                 int sync);

// Deletes key. Returns 1 if key existed, 0 if absent, -1 on error.
int bytecask_del(bytecask_db_t *db, const uint8_t *key, size_t key_len,
                 int sync);

// Deletes all keys in [from, to).  No-op if from >= to.
// Returns 0 on success, -1 on error.
int bytecask_del_range(bytecask_db_t *db,
                       const uint8_t *from, size_t from_len,
                       const uint8_t *to, size_t to_len,
                       int sync);

// ---------------------------------------------------------------------------
// Read operations
// ---------------------------------------------------------------------------

// Looks up key.  On success, *out_val and *out_val_len are set to a
// heap-allocated buffer owned by the caller (free with bytecask_free_buf()).
// Returns 1 if found, 0 if not found, -1 on error.
int bytecask_get(bytecask_db_t *db, const uint8_t *key, size_t key_len,
                 uint8_t **out_val, size_t *out_val_len);

// Returns 1 if key exists (no I/O), 0 if absent.
int bytecask_contains_key(bytecask_db_t *db,
                          const uint8_t *key, size_t key_len);

// ---------------------------------------------------------------------------
// Iteration
//
// Iterates key-value pairs in ascending key order, starting at `from`
// (inclusive).  Pass from=NULL / from_len=0 to iterate from the beginning.
// ---------------------------------------------------------------------------

// Opens a forward iterator positioned at the first key >= from.
// Returns NULL on error.
bytecask_iter_t *bytecask_iter_open(bytecask_db_t *db,
                                    const uint8_t *from, size_t from_len);

// Advances to the next entry.  Returns 1 if a next entry exists, 0 at end.
int bytecask_iter_next(bytecask_iter_t *iter);

// Returns 1 if the iterator has a current valid entry, 0 if exhausted.
int bytecask_iter_valid(const bytecask_iter_t *iter);

// Copies the current key into *out_key / *out_key_len.
// *out_key is owned by the caller; free with bytecask_free_buf().
// Returns 0 on success, -1 on error.
int bytecask_iter_key(const bytecask_iter_t *iter,
                      uint8_t **out_key, size_t *out_key_len);

// Copies the current value into *out_val / *out_val_len.
// *out_val is owned by the caller; free with bytecask_free_buf().
// Returns 0 on success, -1 on error.
int bytecask_iter_value(const bytecask_iter_t *iter,
                        uint8_t **out_val, size_t *out_val_len);

// Frees the iterator and all resources it holds.
void bytecask_iter_free(bytecask_iter_t *iter);

// ---------------------------------------------------------------------------
// Reverse Iteration
//
// Iterates key-value pairs in descending key order, starting at `from`
// (inclusive).  Pass from=NULL / from_len=0 to iterate from the end.
// ---------------------------------------------------------------------------

// Opens a reverse iterator positioned at the last key <= from.
// Returns NULL on error.
bytecask_iter_t *bytecask_riter_open(bytecask_db_t *db,
                                     const uint8_t *from, size_t from_len);

// Advances to the previous entry (reverse iteration).  Returns 1 if a previous entry exists, 0 at start.
int bytecask_riter_next(bytecask_iter_t *iter);

// Opens a reverse iterator on a snapshot, positioned at the last key <= from.
// Returns NULL on error.
bytecask_iter_t *bytecask_snapshot_riter_open(bytecask_snapshot_t *snap,
                                              const uint8_t *from,
                                              size_t from_len);

// ---------------------------------------------------------------------------
// Snapshots
//
// A snapshot is a frozen, read-only view of the database at a point in time.
// Holds open any data files referenced at snapshot time until freed.
// ---------------------------------------------------------------------------

// Creates a snapshot of the current database state.
// Returns NULL on failure; call bytecask_errmsg() for details.
bytecask_snapshot_t *bytecask_snapshot(bytecask_db_t *db);

// Looks up key in the snapshot.  Same semantics as bytecask_get().
int bytecask_snapshot_get(const bytecask_snapshot_t *snap,
                          const uint8_t *key, size_t key_len,
                          uint8_t **out_val, size_t *out_val_len);

// Returns 1 if key exists in the snapshot (no I/O), 0 if absent.
int bytecask_snapshot_contains_key(const bytecask_snapshot_t *snap,
                                   const uint8_t *key, size_t key_len);

// Opens a forward iterator on the snapshot, positioned at the first key >= from.
// Returns NULL on error.
bytecask_iter_t *bytecask_snapshot_iter_open(bytecask_snapshot_t *snap,
                                             const uint8_t *from,
                                             size_t from_len);

// Frees the snapshot and releases all held resources.
void bytecask_snapshot_free(bytecask_snapshot_t *snap);

// ---------------------------------------------------------------------------
// Write plans (conditional atomic writes)
//
// A WritePlan accumulates put/del operations and optional precondition guards.
// bytecask_apply_batch() applies the plan atomically iff all guards hold.
// ---------------------------------------------------------------------------

// Creates an empty write plan without a snapshot.
// Only ensure_present / ensure_absent guards are available.
bytecask_write_plan_t *bytecask_write_plan_new(void);

// Creates an empty write plan backed by a snapshot.
// All guards are available, including ensure_unchanged and ensure_range_unchanged.
// The snapshot is consumed — caller must not use snap after this call.
bytecask_write_plan_t *bytecask_write_plan_new_with_snapshot(
    bytecask_snapshot_t *snap);

// Adds a put operation to the plan.
void bytecask_write_plan_put(bytecask_write_plan_t *plan,
                             const uint8_t *key, size_t key_len,
                             const uint8_t *val, size_t val_len);

// Adds a delete operation to the plan.
void bytecask_write_plan_del(bytecask_write_plan_t *plan,
                             const uint8_t *key, size_t key_len);

// Adds a range delete operation to the plan: deletes all keys in [from, to).
void bytecask_write_plan_del_range(bytecask_write_plan_t *plan,
                                   const uint8_t *from, size_t from_len,
                                   const uint8_t *to, size_t to_len);

// Guard: key must exist at apply time.
void bytecask_write_plan_ensure_present(bytecask_write_plan_t *plan,
                                        const uint8_t *key, size_t key_len);

// Guard: key must be absent at apply time.
void bytecask_write_plan_ensure_absent(bytecask_write_plan_t *plan,
                                       const uint8_t *key, size_t key_len);

// Guard: key must not have been modified since the plan's snapshot.
// Requires a snapshot-backed plan; returns -1 (sets errmsg) otherwise.
int bytecask_write_plan_ensure_unchanged(bytecask_write_plan_t *plan,
                                         const uint8_t *key, size_t key_len);

// Guard: no key in [from, to) must have been modified since the plan's snapshot.
// Requires a snapshot-backed plan; returns -1 (sets errmsg) otherwise.
int bytecask_write_plan_ensure_range_unchanged(bytecask_write_plan_t *plan,
                                               const uint8_t *from,
                                               size_t from_len,
                                               const uint8_t *to,
                                               size_t to_len);

// Frees the write plan.  No-op if plan is NULL.
// Do not call after bytecask_apply_batch() — that consumes the plan.
void bytecask_write_plan_free(bytecask_write_plan_t *plan);

// Applies the plan atomically iff all guards hold and no written key was
// modified since the plan's snapshot.
// Returns 1 if committed, 0 on conflict, -1 on error.
// The plan is consumed (freed) regardless of outcome — caller must not use
// it after this call.
int bytecask_apply_batch(bytecask_db_t *db,
                         bytecask_write_plan_t *plan,
                         int sync);

// ---------------------------------------------------------------------------
// Vacuum
// ---------------------------------------------------------------------------

// Runs one vacuum pass: selects the highest-fragmentation sealed file and
// reclaims dead space.
// Returns 1 if a file was vacuumed, 0 if no file qualified, -1 on error.
int bytecask_vacuum(bytecask_db_t *db);

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------

// Frees a buffer allocated by this API (get, iter_key, iter_value).
void bytecask_free_buf(void *buf);

// ---------------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------------

// Returns a thread-local string describing the last error, or NULL if none.
// Valid until the next bytecask_* call on this thread.
const char *bytecask_errmsg(void);

#ifdef __cplusplus
} // extern "C"
#endif
