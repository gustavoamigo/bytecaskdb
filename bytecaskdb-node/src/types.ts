// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

export interface OpenOptions {
  maxFileBytes?: number;
  failOnCrcErrors?: boolean;
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

export interface CloseableIterator<T> extends Disposable {
  next(): IteratorResult<T>;
  close(): void;
  [Symbol.iterator](): this;
}

export interface Snapshot extends Disposable {
  get(key: string, opts?: ReadOptions): Uint8Array | null;
  containsKey(key: string): boolean;
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
  close(): void;
}

export interface ByteCaskDB extends Disposable {
  get(key: string, opts?: ReadOptions): Uint8Array | null;
  put(key: string, value: string, opts?: WriteOptions): void;
  del(key: string, opts?: WriteOptions): boolean;
  delRange(from: string, to: string, opts?: WriteOptions): void;
  containsKey(key: string): boolean;
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
  close(): void;
}

export interface WritePlanConstructor {
  new(): WritePlan;
  withSnapshot(snap: Snapshot): WritePlan;
}

export interface ByteCaskFactory {
  open(path: string, opts?: OpenOptions): ByteCaskDB;
  WritePlan: WritePlanConstructor;
}
