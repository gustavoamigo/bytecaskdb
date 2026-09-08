// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Gustavo Amigo

// Minimal load + roundtrip smoke test for the native N-API addon. Run via
// `npm run test:smoke:native` after `npm run build:native`. Not part of the
// Vitest suite — this is a fast, dependency-free sanity check that the
// compiled .node file loads and the core API works end to end.

const bc = require("../native/bytecask.node");
const fs = require("node:fs");
const path = require("node:path");
const os = require("node:os");

const dbPath = fs.mkdtempSync(path.join(os.tmpdir(), "bc-native-smoke-"));

try {
  const db = bc.ByteCaskDB.open(dbPath, {});
  db.put(Buffer.from("k"), Buffer.from("v"), {});
  const got = db.get(Buffer.from("k"), {});
  const value = Buffer.from(got).toString("utf8");
  if (value !== "v") {
    throw new Error(`expected "v", got "${value}"`);
  }
  db.close();
  console.log("native smoke test ok");
} finally {
  fs.rmSync(dbPath, { recursive: true, force: true });
}
