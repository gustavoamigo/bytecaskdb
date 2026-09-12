// Shared dataset generation for scripts/bench.mjs. Mirrors the key format,
// value size, batch size, and range length used by benchmarks/engine_bench.cpp
// (the native C++ benchmark) so results are at least directionally comparable
// to the numbers in the root README.

export const VALUE_SIZE = 245; // engine_bench.cpp kValueSize
export const RANGE_LEN = 50; // engine_bench.cpp kRangeLen
export const BATCH_SIZE = 100; // engine_bench.cpp kBatchSize

const PREFIXES = ["user::", "order::", "session::", "invoice::", "product::"];

// Mirrors generate_prefixed_keys() in benchmarks/engine_bench.cpp.
export function generatePrefixedKeys(n) {
  const perPrefix = Math.floor(n / PREFIXES.length);
  const keys = [];
  for (const pfx of PREFIXES) {
    for (let i = 0; i < perPrefix; i++) {
      const hi = (i >>> 16).toString(16).padStart(4, "0");
      const lo = (i >>> 0).toString(16).padStart(12, "0");
      keys.push(`${pfx}018f6e2c-${hi}-7000-8000-${lo}`);
    }
  }
  return keys;
}

// mulberry32 — small deterministic PRNG so the value is stable across runs
// and backends (no crypto or extra dependency needed for a benchmark).
function mulberry32(seed) {
  let a = seed >>> 0;
  return function next() {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export function makeValue() {
  const rng = mulberry32(0x1234abcd);
  let s = "";
  for (let i = 0; i < VALUE_SIZE; i++) {
    s += String.fromCharCode(Math.floor(rng() * 256));
  }
  return s;
}
