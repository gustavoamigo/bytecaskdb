// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

export type Mode = 'leader' | 'follower';

export interface Disposable {
  [Symbol.dispose](): void;
}

export type EntryType = 'put' | 'delete' | 'bulkBegin' | 'bulkEnd' | 'rangeDel';

export interface OpenOptions {
  maxFileBytes?: number;
  failOnCrcErrors?: boolean;
  /** Max key size in bytes (default 4096; hard ceiling 65535). */
  maxKeyBytes?: number;
  /** Max value size in bytes (default 4 MiB; hard ceiling ~4 GiB). */
  maxValueBytes?: number;
  /** Initial engine mode (default 'leader'). */
  initialMode?: Mode;
}

export interface WriteOptions {
  sync?: boolean;
}

export interface ReadOptions {
  verifyChecksums?: boolean;
}

export interface Entry {
  key: Uint8Array;
  value: Uint8Array;
}

export interface DataEntry {
  sequence: number;
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
  getThroughSequence(): number;
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
  delRange(from: string, to: string): void;
  ensurePresent(key: string): void;
  ensureAbsent(key: string): void;
  ensureUnchanged(key: string): void;
  ensureRangeUnchanged(from: string, to: string): void;
  hasSnapshot(): boolean;
  close(): void;
}

export interface ByteCaskDB extends Disposable {
  get(key: string, opts?: ReadOptions): Uint8Array | null;
  put(key: string, value: string, opts?: WriteOptions): void;
  del(key: string, opts?: WriteOptions): boolean;
  delRange(from: string, to: string, opts?: WriteOptions): void;
  containsKey(key: string, opts?: ReadOptions): boolean;
  snapshot(): Snapshot;
  applyBatch(plan: WritePlan, opts?: WriteOptions): boolean;
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
  currentSequence(timeoutMs?: number): number;
  createManifest(): FileManifest;
  changesSince(snap: Snapshot, fromSeq: number): CloseableIterator<DataEntry>;
  ingest(entries: DataEntry[]): void;
  stats(): Record<string, number>;
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
