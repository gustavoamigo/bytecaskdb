#!/usr/bin/env node
// Compares the native (N-API) and WASM (Embind) backends through the
// identical ByteCaskFactory interface (src/types.ts) — the one benchmark
// that actually measures JS <-> binding call overhead, unlike
// benchmarks/engine_bench.cpp (native C++, no JS involved) or
// wasm/bench_main.cpp (the same C++ benchmark cross-compiled to WASM and run
// under NODEFS — still no Embind call in the loop).
//
// Uses tinybench directly (the library Vitest's bench() wrapped in v4) —
// Vitest 5 removed the bench() in-file API entirely, with no replacement.
//
// Dataset methodology (key format, value size, batch size, range length)
// mirrors benchmarks/engine_bench.cpp so results are at least directionally
// comparable to the C++ numbers in the root README.
//
// Usage:
//   node scripts/bench.mjs                        # both backends, all ops
//   node scripts/bench.mjs --filter=Get            # filter by op name
//   BC_BENCH_DATASET_SIZE=100000 node scripts/bench.mjs
//   BC_BENCH_TIME_MS=5000 node scripts/bench.mjs   # longer run (default 500ms)
//
// Prerequisites: npm run build (native addon + WASM module).

import { Bench } from "tinybench";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createNativeBackend } from "../dist/native-backend.js";
import { createWasmBackend } from "../dist/wasm-backend.js";
import { BATCH_SIZE, RANGE_LEN, generatePrefixedKeys, makeValue } from "./bench-dataset.mjs";

function parseArgs(argv) {
  const args = {};
  for (const arg of argv) {
    const [key, value] = arg.replace(/^--/, "").split("=");
    args[key] = value;
  }
  return args;
}

const args = parseArgs(process.argv.slice(2));
const DATASET_SIZE = Number(process.env.BC_BENCH_DATASET_SIZE ?? 20_000);
const TIME_MS = Number(process.env.BC_BENCH_TIME_MS ?? 500);
const KEYS = generatePrefixedKeys(DATASET_SIZE);
const VALUE = makeValue();

// Opens one fresh temp-dir DB, optionally pre-populated with the full
// dataset (nosync — population speed isn't what's being measured).
async function openTempDb(factory, populate) {
  const dir = await mkdtemp(join(tmpdir(), "bytecask-bench-"));
  const db = factory.open(join(dir, "db"));
  if (populate) {
    for (const k of KEYS) db.put(k, VALUE, { sync: false });
  }
  return {
    db,
    cleanup: async () => {
      db.close();
      await rm(dir, { recursive: true, force: true });
    },
  };
}

// Drains up to n entries from a CloseableIterator and closes it.
function takeN(iter, n) {
  let count = 0;
  while (count < n) {
    const { done } = iter.next();
    if (done) break;
    count++;
  }
  iter.close();
  return count;
}

// Runs one operation (a fresh Bench with one task per backend) and returns
// each backend's throughput. State (db, WritePlan, cleanup, iteration
// counter) is opened once per backend via tinybench's own beforeAll/afterAll
// task hooks — verified to fire reliably when tinybench is used directly
// (unlike through Vitest's bench() wrapper, which never invoked them).
async function runOperation(name, backends, populate, iterFn) {
  const bench = new Bench({ time: TIME_MS });
  for (const { name: backendName, factory } of backends) {
    let state;
    bench.add(backendName, () => iterFn(state), {
      beforeAll: async () => {
        const { db, cleanup } = await openTempDb(factory, populate);
        state = { db, cleanup, i: 0, WritePlan: factory.WritePlan };
      },
      afterAll: async () => state.cleanup(),
    });
  }
  await bench.run();
  console.log(`\n=== ${name} ===`);
  console.table(bench.table());
  return bench.tasks.map((t) => ({
    name: t.name,
    opsPerSec: t.result?.throughput.mean ?? NaN,
  }));
}

const OPERATIONS = [
  {
    name: "Put/NoSync",
    populate: false,
    iterFn: (s) => {
      s.db.put(KEYS[s.i % KEYS.length], VALUE, { sync: false });
      s.i++;
    },
  },
  {
    name: "Put/Sync",
    populate: false,
    iterFn: (s) => {
      s.db.put(KEYS[s.i % KEYS.length], VALUE, { sync: true });
      s.i++;
    },
  },
  {
    name: "Get",
    populate: true,
    iterFn: (s) => {
      s.db.get(KEYS[s.i % KEYS.length]);
      s.i++;
    },
  },
  {
    name: "Del/Sync",
    populate: true,
    iterFn: (s) => {
      // Re-insert once we've cycled through all keys, so every del is a
      // hit — matches engine_bench.cpp's BM_Del methodology.
      if (s.i > 0 && s.i % KEYS.length === 0) {
        for (const k of KEYS) s.db.put(k, VALUE, { sync: false });
      }
      s.db.del(KEYS[s.i % KEYS.length], { sync: true });
      s.i++;
    },
  },
  {
    name: "Range50",
    populate: true,
    iterFn: (s) => {
      takeN(s.db.entries(KEYS[s.i % KEYS.length]), RANGE_LEN);
      s.i++;
    },
  },
  {
    name: "MixedBatch/Sync",
    populate: true,
    iterFn: (s) => {
      // 90% put + 10% del in one atomic apply_batch call — matches
      // engine_bench.cpp's BM_MixedBatch.
      const start = s.i * BATCH_SIZE;
      const plan = new s.WritePlan();
      for (let j = 0; j < BATCH_SIZE; j++) {
        const k = KEYS[(start + j) % KEYS.length];
        if (j % 10 === 9) plan.del(k);
        else plan.put(k, VALUE);
      }
      s.db.applyBatch(plan, { sync: true });
      s.i++;
    },
  },
];

async function main() {
  const backends = [
    { name: "native", factory: await createNativeBackend() },
    { name: "wasm", factory: await createWasmBackend() },
  ];

  const filter = args.filter;
  const results = {};
  for (const op of OPERATIONS) {
    if (filter && !op.name.includes(filter)) continue;
    results[op.name] = await runOperation(op.name, backends, op.populate, op.iterFn);
  }

  console.log(`\n=== native vs wasm (ops/s) ===`);
  const header = ["Benchmark".padEnd(20), "native".padEnd(14), "wasm".padEnd(14), "native/wasm"].join(" ");
  console.log(header);
  console.log("-".repeat(header.length));
  for (const [name, rows] of Object.entries(results)) {
    const nativeRow = rows.find((r) => r.name === "native");
    const wasmRow = rows.find((r) => r.name === "wasm");
    const ratio = nativeRow && wasmRow ? nativeRow.opsPerSec / wasmRow.opsPerSec : NaN;
    console.log(
      [
        name.padEnd(20),
        (nativeRow?.opsPerSec ?? NaN).toFixed(0).padEnd(14),
        (wasmRow?.opsPerSec ?? NaN).toFixed(0).padEnd(14),
        Number.isFinite(ratio) ? `${ratio.toFixed(2)}x` : "n/a",
      ].join(" ")
    );
  }
}

main();
