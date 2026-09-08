// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

import { createRequire } from "node:module";
import type { ByteCaskFactory } from "./types.js";
import { applyDisposeWiring } from "./dispose.js";

// The native addon is a CommonJS .node file loaded via require(), even from
// this ESM module — createRequire gives us a require() scoped to this file's
// location so the relative path resolves the same way regardless of the
// caller's own module system.
const require = createRequire(import.meta.url);

export async function createNativeBackend(): Promise<ByteCaskFactory> {
  const addon = require("../native/bytecask.node");
  applyDisposeWiring(addon);
  return {
    open: (path, opts) => addon.ByteCaskDB.open(path, opts ?? {}),
    WritePlan: addon.WritePlan,
  };
}
