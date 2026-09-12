// Compares the native (N-API) and WASM (Embind) backends through the
// identical ByteCaskFactory interface (src/types.ts) — the one place that
// actually measures JS <-> binding call overhead, unlike
// benchmarks/engine_bench.cpp (native C++, no JS involved) or
// wasm/bench_main.cpp (the same C++ benchmark cross-compiled to WASM and run
// under NODEFS — still no Embind call in the loop).
//
// Each operation gets its own describe() with one bench() per backend, so
// Vitest's own comparison report (fastest/slowest, relative %) directly
// answers "is native or wasm faster at X" — no separate hand-rolled
// comparison table needed.
//
// All per-benchmark state (opened DB, populated dataset) is set up via
// top-level await, not inside beforeAll/afterAll: Vitest's benchmark runner
// does not invoke either describe-level beforeAll/afterAll (from 'vitest')
// or tinybench's own per-task FnOptions.beforeAll/afterAll — verified
// empirically (neither ever ran, with no error) against vitest@4.1.10.
// Cleanup is deferred to one file-level afterAll, which does run reliably.
//
// Dataset methodology (key format, value size, batch size, range length)
// mirrors benchmarks/engine_bench.cpp so results are at least directionally
// comparable to the C++ numbers in the root README.
//
// Usage:
//   npx vitest bench                        # both backends, all operations
//   npx vitest bench -t Get                 # filter by benchmark name
//   BC_BENCH_DATASET_SIZE=100000 npx vitest bench
//   BC_BENCH_TIME_MS=5000 npx vitest bench  # run each benchmark longer
//                                            # (tinybench default: 500ms)
//
// Prerequisites: npm run build (native addon + WASM module).

import { afterAll, bench, describe } from "vitest";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { createNativeBackend } from "../../src/native-backend.js";
import { createWasmBackend } from "../../src/wasm-backend.js";
import type { ByteCaskDB, ByteCaskFactory } from "../../src/index.js";
import { BATCH_SIZE, RANGE_LEN, generatePrefixedKeys, makeValue } from "./dataset.js";

const DATASET_SIZE = Number(process.env.BC_BENCH_DATASET_SIZE ?? 20_000);
const KEYS = generatePrefixedKeys(DATASET_SIZE);
const VALUE = makeValue();

// tinybench keeps calling each bench fn until `time` ms elapse (subject to a
// minimum of `iterations` calls) — the third argument to every bench() call
// below. Longer runs mean more samples and tighter percentiles/rme%,
// especially for the fsync-bound Sync benchmarks, which only manage a few
// hundred iterations in the 500ms default.
const BENCH_OPTIONS = { time: Number(process.env.BC_BENCH_TIME_MS ?? 500) };

const FACTORIES: Array<{ name: string; factory: ByteCaskFactory }> = [
  { name: "native", factory: await createNativeBackend() },
  { name: "wasm", factory: await createWasmBackend() },
];

interface BackendFixture {
  name: string;
  factory: ByteCaskFactory;
  db: ByteCaskDB;
}

const cleanups: Array<() => Promise<void>> = [];

afterAll(async () => {
  for (const cleanup of cleanups) await cleanup();
});

// Opens one fresh temp-dir DB per backend, optionally pre-populated with the
// full dataset (nosync — population speed isn't what's being measured).
async function setupBackends(populate: boolean): Promise<BackendFixture[]> {
  return Promise.all(
    FACTORIES.map(async ({ name, factory }) => {
      const dir = await mkdtemp(join(tmpdir(), "bytecask-bench-"));
      const db = factory.open(join(dir, "db"));
      if (populate) {
        for (const k of KEYS) db.put(k, VALUE, { sync: false });
      }
      cleanups.push(async () => {
        db.close();
        await rm(dir, { recursive: true, force: true });
      });
      return { name, factory, db };
    })
  );
}

const putNoSync = await setupBackends(false);
describe("Put/NoSync", () => {
  for (const { name, db } of putNoSync) {
    let i = 0;
    bench(name, () => {
      db.put(KEYS[i % KEYS.length]!, VALUE, { sync: false });
      i++;
    }, BENCH_OPTIONS);
  }
});

const putSync = await setupBackends(false);
describe("Put/Sync", () => {
  for (const { name, db } of putSync) {
    let i = 0;
    bench(name, () => {
      db.put(KEYS[i % KEYS.length]!, VALUE, { sync: true });
      i++;
    }, BENCH_OPTIONS);
  }
});

const get = await setupBackends(true);
describe("Get", () => {
  for (const { name, db } of get) {
    let i = 0;
    bench(name, () => {
      db.get(KEYS[i % KEYS.length]!);
      i++;
    }, BENCH_OPTIONS);
  }
});

const delSync = await setupBackends(true);
describe("Del/Sync", () => {
  for (const { name, db } of delSync) {
    let i = 0;
    bench(name, () => {
      // Re-insert once we've cycled through all keys, so every del is a hit
      // — matches engine_bench.cpp's BM_Del methodology.
      if (i > 0 && i % KEYS.length === 0) {
        for (const k of KEYS) db.put(k, VALUE, { sync: false });
      }
      db.del(KEYS[i % KEYS.length]!, { sync: true });
      i++;
    }, BENCH_OPTIONS);
  }
});

const range50 = await setupBackends(true);
describe("Range50", () => {
  for (const { name, db } of range50) {
    let i = 0;
    bench(name, () => {
      const iter = db.entries(KEYS[i % KEYS.length]!);
      let count = 0;
      while (count < RANGE_LEN) {
        const { done } = iter.next();
        if (done) break;
        count++;
      }
      iter.close();
      i++;
    }, BENCH_OPTIONS);
  }
});

const mixedBatch = await setupBackends(true);
describe("MixedBatch/Sync", () => {
  for (const { name, db, factory } of mixedBatch) {
    let batchIdx = 0;
    bench(name, () => {
      // 90% put + 10% del in one atomic apply_batch call — matches
      // engine_bench.cpp's BM_MixedBatch.
      const plan = new factory.WritePlan();
      const start = batchIdx * BATCH_SIZE;
      for (let j = 0; j < BATCH_SIZE; j++) {
        const k = KEYS[(start + j) % KEYS.length]!;
        if (j % 10 === 9) plan.del(k);
        else plan.put(k, VALUE);
      }
      db.applyBatch(plan, { sync: true });
      batchIdx++;
    }, BENCH_OPTIONS);
  }
});
