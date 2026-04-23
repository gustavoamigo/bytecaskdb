// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import type { ByteCaskFactory } from "./types.js";

export async function createWasmBackend(): Promise<ByteCaskFactory> {
  const __dirname = dirname(fileURLToPath(import.meta.url));
  const wasmPath = join(__dirname, "..", "wasm", "build", "bytecask.mjs");
  const { default: createByteCask } = await import(wasmPath);
  const Module = await createByteCask();
  return {
    open: (path, opts) => Module.ByteCaskDB.open(path, opts ?? {}),
    WritePlan: Module.WritePlan,
  };
}
