// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

export type {
  ByteCaskDB,
  ByteCaskFactory,
  CloseableIterator,
  Entry,
  OpenOptions,
  ReadOptions,
  Snapshot,
  WritePlan,
  WritePlanConstructor,
  WriteOptions,
} from "./types.js";

export { createWasmBackend } from "./wasm-backend.js";

export { createWasmBackend as default } from "./wasm-backend.js";
