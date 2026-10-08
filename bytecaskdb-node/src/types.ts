// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

export type Mode = 'leader' | 'follower';

export interface Disposable {
  [Symbol.dispose](): void;
}

export type EntryType = 'put' | 'delete' | 'bulkBegin' | 'bulkEnd' | 'rangeDel';

// The `code` on every error either backend throws.
//   BC_DEGRADED          a write on a degraded engine; resume() clears it
//   BC_FOLLOWER_MODE     a normal write in follower mode
//   BC_CLOSED            a call on a closed DB, or on a closed or consumed
//                        snapshot or plan
//   BC_INVALID_ARGUMENT  input refused before anything is written: a key,
//                        value or plan over its limit, an empty range, an
//                        option over its ceiling
//   BC_IO                an I/O failure; `errno` holds the system error
//   BC_LOGIC             a call the engine's state does not allow, such as
//                        ingest on a leader or a guard without a snapshot
//   BC_RUNTIME           anything else, data corruption included
export type ErrorCode =
  | 'BC_DEGRADED'
  | 'BC_FOLLOWER_MODE'
  | 'BC_CLOSED'
  | 'BC_INVALID_ARGUMENT'
  | 'BC_IO'
  | 'BC_LOGIC'
  | 'BC_RUNTIME';

export interface ByteCaskError extends Error {
  code: ErrorCode;
  errno?: number;
}

// Selects how data files are read. 'pread' (default) issues pread(2) per
// read; 'mmap' memory-maps sealed files for zero-copy reads; 'bufferPool'
// serves sealed files from a bounded, engine-owned cache — see
// BufferPoolOptions. The WASM backend supports 'pread' and 'bufferPool';
// open() throws for 'mmap' there, since mmap emulation would copy the data
// file into the WASM heap. On WASM a pool hit also skips the call out to
// Node's fs that every pread makes.
export type IoBackend = 'pread' | 'mmap' | 'bufferPool';

export interface BufferPoolOptions {
  /** Total pool footprint in bytes (frames plus index). Must be at least
   * 2x maxFileBytes. Required when ioBackend is 'bufferPool'. */
  capacityBytes: number;
  /** Fill frames with O_DIRECT so the pool is the only consumer of memory
   * for sealed-file data (default true). Falls back to buffered fills per
   * file when the filesystem refuses O_DIRECT. */
  directIo?: boolean;
}

export interface OpenOptions {
  maxFileBytes?: number;
  failOnCrcErrors?: boolean;
  /** Max key size in bytes (default 4096; hard ceiling 65535, open rejects more). */
  maxKeyBytes?: number;
  /** Max value size in bytes (default 4 MiB; hard ceiling 2^28 - 1, open rejects more). */
  maxValueBytes?: number;
  /** Initial engine mode (default 'leader'). */
  initialMode?: Mode;
  /** How data files are read (default 'pread'). */
  ioBackend?: IoBackend;
  /** Only read when ioBackend is 'bufferPool'. */
  bufferPool?: BufferPoolOptions;
}

export interface WriteOptions {
  sync?: boolean;
}

export interface ReadOptions {
  verifyChecksums?: boolean;
}

// Outcome of a committed write. sequence is the highest sequence assigned to
// the write (0n = nothing written). durable is true iff fdatasync confirmed
// it before return. sequence is an exact bigint end-to-end — never a lossy
// double — since JS numbers cannot exactly represent every uint64_t value.
export interface CommitResult {
  sequence: bigint;
  durable: boolean;
}

export interface Entry {
  key: Uint8Array;
  value: Uint8Array;
}

export interface DataEntry {
  sequence: bigint;
  entryType: EntryType;
  key: Uint8Array;
  value: Uint8Array;
}

export interface FileInfo {
  fileId: number;
  dataPath: string;
  hintPath: string;
}

export interface FileManifest extends Disposable {
  getSnapshot(): Snapshot;
  getFiles(): FileInfo[];
  getThroughSequence(): bigint;
  close(): void;
}

export interface CloseableIterator<T> extends Disposable {
  next(): IteratorResult<T>;
  close(): void;
  [Symbol.iterator](): this;
}

export interface Snapshot extends Disposable {
  get(key: string, opts?: ReadOptions): Uint8Array | null;
  containsKey(key: string, opts?: ReadOptions): boolean;
  entries(from: string, opts?: ReadOptions): CloseableIterator<Entry>;
  keys(from: string, opts?: ReadOptions): CloseableIterator<Uint8Array>;
  entriesReverse(from: string, opts?: ReadOptions): CloseableIterator<Entry>;
  keysReverse(from: string, opts?: ReadOptions): CloseableIterator<Uint8Array>;
  close(): void;
}

export interface WritePlan extends Disposable {
  put(key: string, value: string): void;
  del(key: string): void;
  /** Deletes [from, to). Throws if from >= to. */
  delRange(from: string, to: string): void;
  ensurePresent(key: string): void;
  ensureAbsent(key: string): void;
  ensureUnchanged(key: string): void;
  /** Throws if from >= to. */
  ensureRangeUnchanged(from: string, to: string): void;
  hasSnapshot(): boolean;
  close(): void;
}

export interface ByteCaskDB extends Disposable {
  get(key: string, opts?: ReadOptions): Uint8Array | null;
  put(key: string, value: string, opts?: WriteOptions): CommitResult;
  del(key: string, opts?: WriteOptions): CommitResult | null;
  /** Deletes [from, to). Throws, before anything is written, if from >= to. */
  delRange(from: string, to: string, opts?: WriteOptions): CommitResult;
  containsKey(key: string, opts?: ReadOptions): boolean;
  snapshot(): Snapshot;
  applyBatch(plan: WritePlan, opts?: WriteOptions): CommitResult | null;
  entries(from: string, opts?: ReadOptions): CloseableIterator<Entry>;
  keys(from: string, opts?: ReadOptions): CloseableIterator<Uint8Array>;
  entriesReverse(from: string, opts?: ReadOptions): CloseableIterator<Entry>;
  keysReverse(from: string, opts?: ReadOptions): CloseableIterator<Uint8Array>;
  vacuum(): boolean;
  isDegraded(): boolean;
  degradedReason(): string;
  resume(): void;
  mode(): Mode;
  setMode(mode: Mode): void;
  // The single sequence primitive — replaces currentSequence (removed, no
  // alias). minSequence=0n, an already-reached target, or timeoutMs=0 all
  // return immediately without blocking. A positive timeoutMs blocks the
  // calling thread — in Node.js, the event loop — until the durable
  // sequence reaches minSequence or the timeout expires.
  durableSequence(minSequence?: bigint, timeoutMs?: number): bigint;
  createManifest(): FileManifest;
  changesSince(snap: Snapshot, fromSeq: bigint): CloseableIterator<DataEntry>;
  ingest(entries: DataEntry[]): void;
  stats(): Record<string, number>;
  // Makes every write durable, writes the hint files and releases the
  // directory lock and the handle. Throws if an acknowledged write is not
  // durable or the shutdown failed; the DB is closed either way. Idempotent.
  close(): void;
}

export interface WritePlanConstructor {
  new(): WritePlan;
  withSnapshot(snap: Snapshot): WritePlan;
  withLimits(opts: { maxKeyBytes?: number; maxValueBytes?: number }): WritePlan;
}

export interface ByteCaskFactory {
  open(path: string, opts?: OpenOptions): ByteCaskDB;
  WritePlan: WritePlanConstructor;
}
