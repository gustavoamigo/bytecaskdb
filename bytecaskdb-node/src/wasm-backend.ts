// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

import type { ByteCaskFactory } from "./types.js";
import { applyDisposeWiring } from "./dispose.js";

export async function createWasmBackend(): Promise<ByteCaskFactory> {
  return (await loadWasmModule("bytecask")).backend;
}

// Not exported from index.ts: tests load the BYTECASK_TESTING build
// ("bytecask_testing") through it and reach its testing* exports.
export async function loadWasmModule(basename: string) {
  // Use relative path instead of trying to resolve current directory
  const wasmPath = `../wasm/build/${basename}.mjs`;
  const { default: createByteCask } = await import(wasmPath);
  const Module = await createByteCask();
  applyDisposeWiring(Module);
  const backend: ByteCaskFactory = {
    open: (path, opts) => Module.ByteCaskDB.open(path, opts ?? {}),
    WritePlan: Module.WritePlan,
  };
  return { backend, module: Module };
}
