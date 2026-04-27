// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

import type { ByteCaskFactory } from "./types.js";

export async function createWasmBackend(): Promise<ByteCaskFactory> {
  // Use relative path instead of trying to resolve current directory
  const wasmPath = "../wasm/build/bytecask.mjs";
  const { default: createByteCask } = await import(wasmPath);
  const Module = await createByteCask();
  return {
    open: (path, opts) => Module.ByteCaskDB.open(path, opts ?? {}),
    WritePlan: Module.WritePlan,
  };
}
